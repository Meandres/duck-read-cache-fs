#include "column_chunk_planner.hpp"

#include "duckdb/common/helper.hpp"
#include "parquet_layout_registry.hpp"

namespace duckdb {

vector<CacheReadChunk> PlanColumnChunks(const ColumnChunkPlanRequest &request, const string &path) {
	vector<CacheReadChunk> chunks;
	if (request.requested_bytes_to_read == 0 || request.gap_block_size == 0) {
		return chunks;
	}

	const auto &registry = ParquetLayoutRegistry::Get();
	const idx_t req_start = request.requested_start_offset;
	const idx_t req_end = req_start + request.requested_bytes_to_read;
	const idx_t block_size = request.gap_block_size;

	idx_t pos = req_start;
	while (pos < req_end) {
		idx_t region_start = 0;
		idx_t region_end = 0;
		idx_t entry_start = 0;
		idx_t entry_end = 0;

		switch (registry.Locate(path, pos, region_start, region_end)) {
		case LayoutRegion::kExtent:
			entry_start = region_start;
			entry_end = region_end;
			break;
		case LayoutRegion::kGap: {
			const idx_t cell_start = (pos / block_size) * block_size;
			entry_start = MaxValue<idx_t>(region_start, cell_start);
			entry_end = MinValue<idx_t>(region_end, cell_start + block_size);
			break;
		}
		case LayoutRegion::kNoLayout:
			// Layout cleared since the caller checked; let the fixed-block path serve it.
			return {};
		}

		// An entry can never extend past the end of the file: those bytes do not exist to be fetched.
		entry_end = MinValue<idx_t>(entry_end, request.file_size);
		const idx_t copy_from = pos;
		const idx_t copy_to = MinValue<idx_t>(req_end, entry_end);
		if (copy_to <= copy_from) {
			// The request reaches past the end of the file. Do not return what was planned so far: it does not cover
			// the request.
			return {};
		}

		CacheReadChunk chunk;
		chunk.aligned_start_offset = entry_start;
		chunk.chunk_size = entry_end - entry_start;
		chunk.requested_start_offset = copy_from;
		chunk.bytes_to_copy = copy_to - copy_from;
		chunk.requested_start_addr = request.buffer + (copy_from - req_start);
		chunks.push_back(chunk);

		pos = copy_to;
	}
	return chunks;
}

} // namespace duckdb
