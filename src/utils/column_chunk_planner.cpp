#include "column_chunk_planner.hpp"

#include "duckdb/common/helper.hpp"
#include "parquet_layout_registry.hpp"

namespace duckdb {

vector<CacheReadChunk> PlanColumnChunks(const ColumnChunkPlanRequest &request, const string &path) {
	vector<CacheReadChunk> chunks;
	if (request.requested_bytes_to_read == 0) {
		return chunks;
	}

	auto &registry = ParquetLayoutRegistry::Get();
	const idx_t req_start = request.requested_start_offset;
	const idx_t req_end = req_start + request.requested_bytes_to_read;
	const idx_t gap_cap = request.max_gap_entry_size > 0 ? request.max_gap_entry_size : request.requested_bytes_to_read;

	idx_t pos = req_start;
	while (pos < req_end) {
		idx_t entry_start = 0;
		idx_t entry_size = 0;

		FileExtent extent;
		if (registry.Find(path, pos, extent)) {
			// Cache the whole column chunk, even the part of it this request
			// does not need -- that is the point: the next request for another
			// slice of the same chunk becomes a hit.
			entry_start = extent.offset;
			entry_size = extent.size;
		} else {
			// No extent covers [pos]: synthesise an entry running up to the
			// next extent, so a cached entry never straddles a chunk boundary
			// and the two strategies cannot produce overlapping keys.
			idx_t next_start = 0;
			const idx_t gap_end =
			    registry.NextStart(path, pos, next_start) ? next_start : MaxValue<idx_t>(req_end, request.file_size);
			entry_start = pos;
			entry_size = MinValue<idx_t>(gap_cap, gap_end - pos);
		}

		if (request.file_size > 0) {
			if (entry_start >= request.file_size) {
				break;
			}
			entry_size = MinValue<idx_t>(entry_size, request.file_size - entry_start);
		}
		if (entry_size == 0) {
			break;
		}

		const idx_t entry_end = entry_start + entry_size;
		const idx_t copy_from = MaxValue<idx_t>(pos, entry_start);
		const idx_t copy_to = MinValue<idx_t>(req_end, entry_end);
		if (copy_to <= copy_from) {
			break; // no forward progress; refuse to loop
		}

		CacheReadChunk chunk;
		chunk.aligned_start_offset = entry_start;
		chunk.chunk_size = entry_size;
		chunk.requested_start_offset = copy_from;
		chunk.bytes_to_copy = copy_to - copy_from;
		chunk.requested_start_addr = request.buffer + (copy_from - req_start);
		chunks.push_back(chunk);

		pos = copy_to;
	}

	return chunks;
}

} // namespace duckdb
