// Per-file Parquet column-chunk extents, used to cache at column-chunk
// granularity instead of on a fixed block grid.
//
// The extension sits below the Parquet reader and cannot ask it anything, so
// the layout is pushed in from SQL:
//
//   SELECT cache_httpfs_register_parquet_layout('s3://bucket/lineitem.parquet');
//
// which reads the footer through `parquet_metadata` and records one extent per
// column chunk. Keeping the extraction in SQL means this extension never has to
// link or track the Parquet format itself.

#pragma once

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

// Process-wide, matching the extension's existing model where the in-memory
// block cache is shared across all cache filesystems.
class ParquetLayoutRegistry {
public:
	static ParquetLayoutRegistry &Get();

	// [extents] need not be sorted; they are sorted and overlaps dropped.
	// Returns the number of extents actually stored.
	idx_t Register(const string &path, vector<FileExtent> extents);

	bool HasLayout(const string &path) const;

	// Extent containing [offset], if any.
	bool Find(const string &path, idx_t offset, FileExtent &out) const;

	// Start of the first extent at or after [offset]; returns false if none.
	bool NextStart(const string &path, idx_t offset, idx_t &out) const;

	// (path, extent count, total bytes covered) for every registered file.
	vector<std::tuple<string, idx_t, idx_t>> List() const;

	void Clear();
	void Clear(const string &path);

private:
	mutable std::mutex lock;
	std::unordered_map<string, vector<FileExtent>> layouts;
};

// Read the footer of [path] via `parquet_metadata` and register one extent per
// column chunk. Returns the number of extents registered.
idx_t RegisterParquetLayoutFromFooter(DatabaseInstance &db, const string &path);

} // namespace duckdb
