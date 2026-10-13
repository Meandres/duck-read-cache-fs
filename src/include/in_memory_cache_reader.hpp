// A filesystem wrapper, which performs in-memory cache for read operations.

#pragma once

#include "base_cache_reader.hpp"
#include "cache_filesystem_config.hpp"
#include "cache_read_chunk.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "in_mem_cache_block.hpp"
#include "in_mem_cache_data_entry.hpp"
#include "in_memory_data_cache_storage.hpp"
#include "mutex.hpp"

#include <future>
#include <map>

namespace duckdb {

// Forward declarations.
struct CacheHttpfsInstanceState;
struct InMemoryCacheReaderConfig;

class InMemoryCacheReader final : public BaseCacheReader {
public:
	// Constructor: config values are read from instance state at runtime (with defaults as fallback).
	explicit InMemoryCacheReader(weak_ptr<CacheHttpfsInstanceState> instance_state_p)
	    : BaseCacheReader(std::move(instance_state_p)) {
	}
	~InMemoryCacheReader() override = default;

	string GetName() const override {
		return *IN_MEM_CACHE_READER_NAME;
	}

	void ClearCache() override;
	void ClearCache(const string &fname) override;
	void ReadAndCache(FileHandle &handle, char *buffer, uint64_t requested_start_offset,
	                  uint64_t requested_bytes_to_read, uint64_t file_size) override;
	vector<DataCacheEntryInfo> GetCacheEntriesInfo() const override;
	void RemapInMemoryDataBlocksForNewBlockSize(idx_t new_block_size) override;

private:
	// Process a single cache read chunk in a worker thread.
	void ProcessCacheReadChunk(FileHandle &handle, const string &version_tag, CacheReadChunk cache_read_chunk);

	// Serve [chunks], which cover one read request contiguously and in order.
	void ReadChunks(FileHandle &handle, const string &version_tag, const vector<CacheReadChunk> &chunks,
	                idx_t subrequest_count, const InMemoryCacheReaderConfig &config,
	                const shared_ptr<CacheHttpfsInstanceState> &state);
	// Same, fetching each run of adjacent missing chunks with as few remote reads as the read size cap allows.
	void ReadChunksCoalesced(FileHandle &handle, const string &version_tag, const vector<CacheReadChunk> &chunks,
	                         const InMemoryCacheReaderConfig &config, const shared_ptr<CacheHttpfsInstanceState> &state);

	// An entry being fetched by some thread; others missing on it wait for [done] instead of fetching it again.
	struct InFlightRead {
		std::promise<void> promise;
		std::shared_future<void> done = promise.get_future().share();
	};
	// Removes [key] from the in-flight table and wakes its waiters; the entry is in storage by then, unless the
	// fetch failed.
	void CompleteInFlight(const InMemCacheBlock &key, InFlightRead &in_flight);

	// Once flag to guard against cache's initialization.
	std::once_flag cache_init_flag;
	// In-memory cache to store blocks; late initialized after first access.
	shared_ptr<InMemoryDataCacheStorage> storage;

	concurrency::mutex in_flight_mutex;
	std::map<InMemCacheBlock, shared_ptr<InFlightRead>, InMemCacheBlockLess> in_flight
	    DUCKDB_GUARDED_BY(in_flight_mutex);
};

} // namespace duckdb
