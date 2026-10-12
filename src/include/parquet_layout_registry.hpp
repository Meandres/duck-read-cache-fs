// Per-file Parquet column-chunk extents, used to size in-memory cache entries to
// column chunks instead of a fixed block grid.
//
// The extension sits below the Parquet reader and cannot ask it anything, so the
// layout is pushed in from SQL:
//
//   SELECT cache_httpfs_register_parquet_layout('s3://bucket/lineitem.parquet');
//   SELECT cache_httpfs_register_parquet_layout(file) FROM glob('s3://bucket/*.parquet');
//
// which reads the footer through `parquet_metadata` and records one extent per
// column chunk. Keeping the extraction in SQL means this extension never has to
// link or track the Parquet format itself.

#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

// Forward declaration.
class DatabaseInstance;

struct FileExtent {
	idx_t offset = 0;
	idx_t size = 0;

	idx_t End() const {
		return offset + size;
	}
};

// What lies at a given offset of a file with a registered layout.
enum class LayoutRegion : uint8_t {
	// The file has no registered layout.
	kNoLayout,
	// Inside a column chunk; [start, end) are the chunk's bounds.
	kExtent,
	// Between column chunks (e.g. the footer); [start, end) are the gap's bounds, the end being the next chunk's start,
	// or the maximum offset if no chunk follows.
	kGap,
};

// Process-wide, so a layout registered through any connection is seen by every cache filesystem.
class ParquetLayoutRegistry {
public:
	static ParquetLayoutRegistry &Get();

	// [extents] need not be sorted; they are sorted and zero-length or overlapping ones dropped.
	// Replaces any layout already registered for [path]. Returns the number of extents stored.
	idx_t Register(const string &path, vector<FileExtent> extents);

	bool HasLayout(const string &path) const;

	// Classify [offset] and return the bounds of the extent or gap containing it.
	LayoutRegion Locate(const string &path, idx_t offset, idx_t &start, idx_t &end) const;

	// Number of files with a registered layout.
	idx_t FileCount() const;

	void Clear();

private:
	mutable std::mutex lock;
	std::unordered_map<string, vector<FileExtent>> layouts;
};

// Read the footer of [path] via `parquet_metadata` and register one extent per column chunk. Returns the number of
// extents registered.
idx_t RegisterParquetLayoutFromFooter(DatabaseInstance &db, const string &path);

} // namespace duckdb
