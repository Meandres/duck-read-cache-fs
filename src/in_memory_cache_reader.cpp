#include "in_memory_cache_reader.hpp"

#include "cache_filesystem.hpp"
#include "cache_filesystem_logger.hpp"
#include "cache_httpfs_instance_state.hpp"
#include "cache_read_chunk.hpp"
#include "crypto.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/database.hpp"
#include "in_mem_cache_remap.hpp"
#include "in_memory_data_cache_storage.hpp"
#include "parquet_layout_registry.hpp"
#include "utils/include/chunk_utils.hpp"
#include "utils/include/column_chunk_planner.hpp"
#include "utils/include/page_aligned_data_chunk.hpp"
#include "utils/include/parallel_executor.hpp"
#include "utils/include/thread_utils.hpp"

#include <cstdint>
#include <utility>
namespace duckdb {

// Runtime config for InMemoryCacheReader (fetched from instance state or defaults)
struct InMemoryCacheReaderConfig {
	idx_t max_cache_block_count = DEFAULT_MAX_IN_MEM_CACHE_BLOCK_COUNT;
	idx_t max_cache_bytes = DEFAULT_MAX_IN_MEM_CACHE_BYTES;
	idx_t cache_block_timeout_millisec = DEFAULT_IN_MEM_BLOCK_CACHE_TIMEOUT_MILLISEC;
	idx_t cache_block_size = DEFAULT_CACHE_BLOCK_SIZE;
	uint64_t max_subrequest_count = DEFAULT_MAX_SUBREQUEST_COUNT;
	bool enable_cache_validation = DEFAULT_ENABLE_CACHE_VALIDATION;
	string in_mem_cache_storage = *DEFAULT_IN_MEM_CACHE_STORAGE;
	string chunking = *DEFAULT_CHUNKING;
	bool coalesce_misses = DEFAULT_COALESCE_MISSES;
	idx_t max_coalesced_read_bytes = DEFAULT_MAX_COALESCED_READ_BYTES;
};

namespace {

// Get runtime config from instance state (returns copy with defaults if unavailable)
InMemoryCacheReaderConfig GetConfig(const CacheHttpfsInstanceState &instance_state) {
	return InMemoryCacheReaderConfig {
	    .max_cache_block_count = instance_state.config.max_in_mem_cache_block_count,
	    .max_cache_bytes = instance_state.config.max_in_mem_cache_bytes,
	    .cache_block_timeout_millisec = instance_state.config.in_mem_cache_block_timeout_millisec,
	    .cache_block_size = instance_state.config.cache_block_size,
	    .max_subrequest_count = instance_state.config.max_subrequest_count,
	    .enable_cache_validation = instance_state.config.enable_cache_validation,
	    .in_mem_cache_storage = instance_state.config.in_mem_cache_storage,
	    .chunking = instance_state.config.chunking,
	    .coalesce_misses = instance_state.config.coalesce_misses,
	    .max_coalesced_read_bytes = instance_state.config.max_coalesced_read_bytes,
	};
}

} // namespace

void InMemoryCacheReader::ProcessCacheReadChunk(FileHandle &handle, const string &version_tag,
                                                CacheReadChunk cache_read_chunk) {
	SetThreadName("RdCachRdThd");

	auto &cache_handle = handle.Cast<CacheFileSystemHandle>();
	auto state = instance_state.lock();
	auto &collector = GetProfileCollectorOrThrow(state, cache_handle.GetConnectionId());

	// Check local cache first, see if we could do a cached read.
	const InMemCacheBlock block_key {handle.GetPath(), cache_read_chunk.aligned_start_offset,
	                                 cache_read_chunk.chunk_size};
	auto pinned = storage->Get(block_key, version_tag);

	if (pinned) {
		collector.RecordCacheAccess(CacheEntity::kData, CacheAccess::kCacheHit, cache_read_chunk.bytes_to_copy);
		DUCKDB_LOG_OPEN_CACHE_HIT((handle));
		cache_read_chunk.CopyBufferToRequestedMemory(pinned->Data());
		return;
	}

	// We suffer a cache loss, fallback to remote access then local filesystem write.
	collector.RecordCacheAccess(CacheEntity::kData, CacheAccess::kCacheMiss, cache_read_chunk.bytes_to_copy);
	DUCKDB_LOG_OPEN_CACHE_MISS((handle));
	auto content = AllocatePageAlignedChunk(cache_read_chunk.chunk_size);
	auto &in_mem_cache_handle = handle.Cast<CacheFileSystemHandle>();
	auto *internal_filesystem = in_mem_cache_handle.GetInternalFileSystem();

	{
		const auto latency_guard = collector.RecordOperationStart(IoOperation::kRead);
		internal_filesystem->Read(*in_mem_cache_handle.internal_file_handle, content.data(),
		                          cache_read_chunk.chunk_size, cache_read_chunk.aligned_start_offset);
		content.length = cache_read_chunk.chunk_size;
	}

	// Copy to destination buffer.
	cache_read_chunk.CopyBufferToRequestedMemory(content);

	storage->Put(std::move(block_key), std::move(content), version_tag);
}

void InMemoryCacheReader::ReadChunks(FileHandle &handle, const string &version_tag,
                                     const vector<CacheReadChunk> &chunks, idx_t subrequest_count,
                                     const InMemoryCacheReaderConfig &config,
                                     const shared_ptr<CacheHttpfsInstanceState> &state) {
	if (config.coalesce_misses) {
		ReadChunksCoalesced(handle, version_tag, chunks, config, state);
		return;
	}

	const auto task_count = GetThreadCountForSubrequests(subrequest_count, config.max_subrequest_count);
	auto parallel_executor = CreateParallelExecutor(state->db_instance, state->config.parallel_read_mode, task_count);
	for (const auto &chunk : chunks) {
		parallel_executor->Schedule(
		    [this, &handle, &version_tag, chunk]() { ProcessCacheReadChunk(handle, version_tag, chunk); });
	}
	parallel_executor->WaitAll();
}

void InMemoryCacheReader::CompleteInFlight(const InMemCacheBlock &key, InFlightRead &read) {
	{
		const concurrency::lock_guard<concurrency::mutex> lck(in_flight_mutex);
		in_flight.erase(key);
	}
	read.promise.set_value();
}

void InMemoryCacheReader::ReadChunksCoalesced(FileHandle &handle, const string &version_tag,
                                              const vector<CacheReadChunk> &chunks,
                                              const InMemoryCacheReaderConfig &config,
                                              const shared_ptr<CacheHttpfsInstanceState> &state) {
	auto &cache_handle = handle.Cast<CacheFileSystemHandle>();
	auto &collector = GetProfileCollectorOrThrow(state, cache_handle.GetConnectionId());
	auto *internal_filesystem = cache_handle.GetInternalFileSystem();
	const auto &path = handle.GetPath();

	auto serve_hit = [&](const CacheReadChunk &chunk, const InMemCacheBlock &key) {
		auto pinned = storage->Get(key, version_tag);
		if (!pinned) {
			return false;
		}
		collector.RecordCacheAccess(CacheEntity::kData, CacheAccess::kCacheHit, chunk.bytes_to_copy);
		DUCKDB_LOG_OPEN_CACHE_HIT((handle));
		chunk.CopyBufferToRequestedMemory(pinned->Data());
		return true;
	};

	// Serve hits, claim the misses nobody is fetching, and note the ones somebody else is.
	vector<shared_ptr<InFlightRead>> claims(chunks.size());
	vector<std::pair<idx_t, shared_ptr<InFlightRead>>> others;
	for (idx_t idx = 0; idx < chunks.size(); ++idx) {
		const auto &chunk = chunks[idx];
		const InMemCacheBlock key {path, chunk.aligned_start_offset, chunk.chunk_size};
		if (serve_hit(chunk, key)) {
			continue;
		}
		shared_ptr<InFlightRead> claim;
		{
			const concurrency::lock_guard<concurrency::mutex> lck(in_flight_mutex);
			auto iter = in_flight.find(key);
			if (iter != in_flight.end()) {
				others.emplace_back(idx, iter->second);
				continue;
			}
			claim = make_shared_ptr<InFlightRead>();
			in_flight.emplace(key, claim);
		}
		// Its fetch may have completed between the lookup and the claim.
		if (serve_hit(chunk, key)) {
			CompleteInFlight(key, *claim);
			continue;
		}
		claims[idx] = std::move(claim);
	}

	// Runs of claimed chunks that are adjacent in the file; each is fetched into one buffer.
	struct Run {
		idx_t first;
		idx_t last;
		PageAlignedDataChunk buffer;
	};
	vector<Run> runs;
	for (idx_t idx = 0; idx < chunks.size(); ++idx) {
		if (!claims[idx]) {
			continue;
		}
		if (!runs.empty() && runs.back().last + 1 == idx &&
		    chunks[idx - 1].aligned_start_offset + chunks[idx - 1].chunk_size == chunks[idx].aligned_start_offset) {
			runs.back().last = idx;
		} else {
			runs.push_back(Run {idx, idx, {}});
		}
	}

	auto release_claims = [&]() {
		for (idx_t idx = 0; idx < chunks.size(); ++idx) {
			if (claims[idx]) {
				CompleteInFlight(InMemCacheBlock {path, chunks[idx].aligned_start_offset, chunks[idx].chunk_size},
				                 *claims[idx]);
				claims[idx] = nullptr;
			}
		}
	};

	try {
		idx_t read_count = 0;
		for (auto &run : runs) {
			const idx_t run_bytes = chunks[run.last].aligned_start_offset + chunks[run.last].chunk_size -
			                        chunks[run.first].aligned_start_offset;
			run.buffer = AllocatePageAlignedChunk(run_bytes);
			run.buffer.length = run_bytes;
			read_count += (run_bytes + config.max_coalesced_read_bytes - 1) / config.max_coalesced_read_bytes;
		}
		const auto task_count = GetThreadCountForSubrequests(read_count, config.max_subrequest_count);
		auto executor = runs.empty() ? nullptr
		                             : CreateParallelExecutor(state->db_instance, state->config.parallel_read_mode,
		                                                      task_count);
		for (auto &run : runs) {
			const idx_t run_start = chunks[run.first].aligned_start_offset;
			for (idx_t offset = 0; offset < run.buffer.length; offset += config.max_coalesced_read_bytes) {
				const idx_t bytes = MinValue<idx_t>(config.max_coalesced_read_bytes, run.buffer.length - offset);
				char *dest = run.buffer.data() + offset;
				executor->Schedule([&collector, internal_filesystem, &cache_handle, dest, bytes, run_start, offset]() {
					SetThreadName("RdCachRdThd");
					const auto latency_guard = collector.RecordOperationStart(IoOperation::kRead);
					internal_filesystem->Read(*cache_handle.internal_file_handle, dest, bytes, run_start + offset);
				});
			}
		}
		if (executor) {
			executor->WaitAll();
		}

		// Cut each run into its entries.
		for (auto &run : runs) {
			const idx_t run_start = chunks[run.first].aligned_start_offset;
			for (idx_t idx = run.first; idx <= run.last; ++idx) {
				const auto &chunk = chunks[idx];
				PageAlignedDataChunk content;
				if (run.first == run.last) {
					content = std::move(run.buffer);
				} else {
					content = AllocatePageAlignedChunk(chunk.chunk_size);
					content.length = chunk.chunk_size;
					run.buffer.CopyTo(content.data(), chunk.aligned_start_offset - run_start, chunk.chunk_size);
				}
				collector.RecordCacheAccess(CacheEntity::kData, CacheAccess::kCacheMiss, chunk.bytes_to_copy);
				DUCKDB_LOG_OPEN_CACHE_MISS((handle));
				chunk.CopyBufferToRequestedMemory(content);
				InMemCacheBlock key {path, chunk.aligned_start_offset, chunk.chunk_size};
				storage->Put(key, std::move(content), version_tag);
				CompleteInFlight(key, *claims[idx]);
				claims[idx] = nullptr;
			}
		}
	} catch (...) {
		release_claims();
		throw;
	}

	// Only now wait for other threads' fetches: ours are all complete, so no two readers can wait on each other.
	for (auto &other : others) {
		other.second->done.wait();
		const auto &chunk = chunks[other.first];
		if (!serve_hit(chunk, InMemCacheBlock {path, chunk.aligned_start_offset, chunk.chunk_size})) {
			// Their fetch failed, or the entry was already evicted.
			ProcessCacheReadChunk(handle, version_tag, chunk);
		}
	}
}

void InMemoryCacheReader::ReadAndCache(FileHandle &handle, char *buffer, idx_t requested_start_offset,
                                       idx_t requested_bytes_to_read, idx_t file_size) {
	if (requested_bytes_to_read == 0) {
		return;
	}

	auto state = instance_state.lock();
	const auto config = GetConfig(*state);

	std::call_once(cache_init_flag, [this, &config, &state]() {
		storage = BuildInMemoryDataCacheStorage(config.in_mem_cache_storage, state->db_instance,
		                                        config.max_cache_block_count, config.cache_block_timeout_millisec,
		                                        config.max_cache_bytes);
	});

	const idx_t block_size = config.cache_block_size;

	// Column-chunk-sized entries, for files whose Parquet layout is registered. Everything below this branch is the
	// unmodified fixed-block path, which still serves every other file -- and this one too whenever the plan comes back
	// empty, so that a request is always served in full.
	if (config.chunking == *COLUMN_CHUNK_CHUNKING && ParquetLayoutRegistry::Get().HasLayout(handle.GetPath())) {
		const ColumnChunkPlanRequest plan_request {
		    .buffer = buffer,
		    .requested_start_offset = requested_start_offset,
		    .requested_bytes_to_read = requested_bytes_to_read,
		    .file_size = file_size,
		    .gap_block_size = block_size,
		};
		const auto planned_chunks = PlanColumnChunks(plan_request, handle.GetPath());
		if (!planned_chunks.empty()) {
			const string version_tag =
			    config.enable_cache_validation ? handle.Cast<CacheFileSystemHandle>().GetVersionTag() : "";
			idx_t total_bytes_to_cache = 0;
			for (const auto &chunk : planned_chunks) {
				total_bytes_to_cache += chunk.chunk_size;
			}
			ReadChunks(handle, version_tag, planned_chunks, planned_chunks.size(), config, state);

			auto &collector = GetProfileCollectorOrThrow(state, handle.Cast<CacheFileSystemHandle>().GetConnectionId());
			collector.RecordActualCacheRead(/*cache_size=*/total_bytes_to_cache,
			                                /*actual_bytes=*/requested_bytes_to_read);
			return;
		}
	}

	const ReadRequestParams read_params {
	    .requested_start_offset = requested_start_offset,
	    .requested_bytes_to_read = requested_bytes_to_read,
	    .block_size = block_size,
	};

	const ChunkAlignmentInfo alignment_info = CalculateChunkAlignment(read_params);

	// Indicate the memory address to copy to for each IO operation
	char *addr_to_write = buffer;
	// Used to calculate bytes to copy for last chunk.
	idx_t already_read_bytes = 0;

	// Get file-level metadata once before processing chunks.
	string version_tag = config.enable_cache_validation ? handle.Cast<CacheFileSystemHandle>().GetVersionTag() : "";

	// To improve IO performance, we split requested bytes (after alignment) into multiple chunks and fetch them in
	// parallel.
	idx_t total_bytes_to_cache = 0;
	vector<CacheReadChunk> chunks;
	chunks.reserve(alignment_info.subrequest_count);
	for (idx_t io_start_offset = alignment_info.aligned_start_offset;
	     io_start_offset <= alignment_info.aligned_last_chunk_offset; io_start_offset += block_size) {
		// No bytes to read for the last chunk.
		if (io_start_offset == file_size) {
			continue;
		}

		CacheReadChunk cache_read_chunk;
		cache_read_chunk.requested_start_addr = addr_to_write;
		cache_read_chunk.aligned_start_offset = io_start_offset;
		cache_read_chunk.requested_start_offset = requested_start_offset;

		// Implementation-wise, middle chunks are easy to handle -- read in [block_size], and copy the whole chunk to
		// the requested memory address; but the first and last chunk require special handling.
		// For first chunk, requested start offset might not be aligned with block size; for the last chunk, we might
		// not need to copy the whole [block_size] of memory.
		//
		// Case-1: If there's only one chunk, which serves as both the first chunk and the last one.
		if (io_start_offset == alignment_info.aligned_start_offset &&
		    io_start_offset == alignment_info.aligned_last_chunk_offset) {
			cache_read_chunk.chunk_size = MinValue<idx_t>(block_size, file_size - io_start_offset);
			cache_read_chunk.bytes_to_copy = requested_bytes_to_read;
		}
		// Case-2: First chunk.
		else if (io_start_offset == alignment_info.aligned_start_offset) {
			const idx_t delta_offset = requested_start_offset - alignment_info.aligned_start_offset;
			addr_to_write += block_size - delta_offset;
			already_read_bytes += block_size - delta_offset;

			cache_read_chunk.chunk_size = block_size;
			cache_read_chunk.bytes_to_copy = block_size - delta_offset;
		}
		// Case-3: Last chunk.
		else if (io_start_offset == alignment_info.aligned_last_chunk_offset) {
			cache_read_chunk.chunk_size = MinValue<idx_t>(block_size, file_size - io_start_offset);
			cache_read_chunk.bytes_to_copy = requested_bytes_to_read - already_read_bytes;
		}
		// Case-4: Middle chunks.
		else {
			addr_to_write += block_size;
			already_read_bytes += block_size;

			cache_read_chunk.bytes_to_copy = block_size;
			cache_read_chunk.chunk_size = block_size;
		}
		total_bytes_to_cache += cache_read_chunk.chunk_size;

		// Update read offset for next chunk read.
		requested_start_offset = io_start_offset + block_size;

		chunks.push_back(cache_read_chunk);
	}

	ReadChunks(handle, version_tag, chunks, alignment_info.subrequest_count, config, state);

	// Record "bytes to read" and "bytes to cache".
	auto &cache_handle_for_profile = handle.Cast<CacheFileSystemHandle>();
	auto state_for_profile = instance_state.lock();
	auto &collector = GetProfileCollectorOrThrow(state_for_profile, cache_handle_for_profile.GetConnectionId());
	collector.RecordActualCacheRead(/*cache_size=*/total_bytes_to_cache,
	                                /*actual_bytes=*/requested_bytes_to_read);
}

vector<DataCacheEntryInfo> InMemoryCacheReader::GetCacheEntriesInfo() const {
	if (storage == nullptr) {
		return {};
	}

	auto keys = storage->Keys();
	vector<DataCacheEntryInfo> cache_entries_info;
	cache_entries_info.reserve(keys.size());
	for (auto &cur_key : keys) {
		cache_entries_info.emplace_back(DataCacheEntryInfo {
		    .cache_filepath = "(no disk cache)",
		    .original_remote_path = std::move(cur_key.fname),
		    .start_offset = cur_key.start_off,
		    .end_offset = cur_key.start_off + cur_key.blk_size,
		    .cache_type = "in-mem",
		});
	}
	return cache_entries_info;
}

void InMemoryCacheReader::ClearCache() {
	if (storage != nullptr) {
		storage->Clear();
	}
}

void InMemoryCacheReader::ClearCache(const string &fname) {
	if (storage != nullptr) {
		// Start from the first block key for this file (ordered by fname, start_off, blk_size).
		const InMemCacheBlock start_key {fname, /*start_off_p=*/0, /*blk_size_p=*/0};
		storage->Clear(start_key, [&fname](const InMemCacheBlock &block) { return block.fname == fname; });
	}
}

void InMemoryCacheReader::RemapInMemoryDataBlocksForNewBlockSize(idx_t new_block_size) {
	if (storage == nullptr) {
		return;
	}
	auto taken = storage->Take();
	// TODO: pass known remote file sizes (e.g. from metadata cache) so remap matches EOF behavior of real reads.
	auto rebuilt = RemapInMemCacheEntries(std::move(taken), new_block_size, /*file_size_by_path=*/ {});
	for (auto &kv : rebuilt) {
		auto &entry = kv.second;
		storage->Put(std::move(kv.first), std::move(entry->data), std::move(entry->version_tag));
	}
}

} // namespace duckdb
