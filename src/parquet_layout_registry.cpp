#include "parquet_layout_registry.hpp"

#include <algorithm>
#include <tuple>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

ParquetLayoutRegistry &ParquetLayoutRegistry::Get() {
	static ParquetLayoutRegistry instance;
	return instance;
}

idx_t ParquetLayoutRegistry::Register(const string &path, vector<FileExtent> extents) {
	std::sort(extents.begin(), extents.end(),
	          [](const FileExtent &a, const FileExtent &b) { return a.offset < b.offset; });

	// Drop zero-length and overlapping extents: the planner assumes extents are
	// disjoint and ascending, and an overlap would let the same bytes be cached
	// under two different keys.
	vector<FileExtent> cleaned;
	cleaned.reserve(extents.size());
	for (auto &extent : extents) {
		if (extent.size == 0) {
			continue;
		}
		if (!cleaned.empty() && extent.offset < cleaned.back().End()) {
			continue;
		}
		cleaned.push_back(extent);
	}

	const idx_t count = cleaned.size();
	std::lock_guard<std::mutex> guard(lock);
	layouts[path] = std::move(cleaned);
	return count;
}

bool ParquetLayoutRegistry::HasLayout(const string &path) const {
	std::lock_guard<std::mutex> guard(lock);
	return layouts.find(path) != layouts.end();
}

bool ParquetLayoutRegistry::Find(const string &path, idx_t offset, FileExtent &out) const {
	std::lock_guard<std::mutex> guard(lock);
	auto it = layouts.find(path);
	if (it == layouts.end()) {
		return false;
	}
	auto &extents = it->second;
	// First extent starting after [offset], then step back one.
	auto pos = std::upper_bound(extents.begin(), extents.end(), offset,
	                            [](idx_t value, const FileExtent &e) { return value < e.offset; });
	if (pos == extents.begin()) {
		return false;
	}
	--pos;
	if (offset < pos->End()) {
		out = *pos;
		return true;
	}
	return false;
}

bool ParquetLayoutRegistry::NextStart(const string &path, idx_t offset, idx_t &out) const {
	std::lock_guard<std::mutex> guard(lock);
	auto it = layouts.find(path);
	if (it == layouts.end()) {
		return false;
	}
	auto &extents = it->second;
	auto pos = std::lower_bound(extents.begin(), extents.end(), offset,
	                            [](const FileExtent &e, idx_t value) { return e.offset < value; });
	if (pos == extents.end()) {
		return false;
	}
	out = pos->offset;
	return true;
}

vector<std::tuple<string, idx_t, idx_t>> ParquetLayoutRegistry::List() const {
	std::lock_guard<std::mutex> guard(lock);
	vector<std::tuple<string, idx_t, idx_t>> out;
	out.reserve(layouts.size());
	for (auto &kv : layouts) {
		idx_t covered = 0;
		for (auto &extent : kv.second) {
			covered += extent.size;
		}
		out.emplace_back(kv.first, static_cast<idx_t>(kv.second.size()), covered);
	}
	return out;
}

void ParquetLayoutRegistry::Clear() {
	std::lock_guard<std::mutex> guard(lock);
	layouts.clear();
}

void ParquetLayoutRegistry::Clear(const string &path) {
	std::lock_guard<std::mutex> guard(lock);
	layouts.erase(path);
}

idx_t RegisterParquetLayoutFromFooter(DatabaseInstance &db, const string &path) {
	// Mirrors duckdb::ColumnReader::FileOffset: a column chunk starts at the
	// earliest of its dictionary, index and data page offsets. The unset ones
	// come back as NULL or 0, hence the nullif/coalesce dance.
	const string escaped = StringUtil::Replace(path, "'", "''");
	const string query = StringUtil::Format(
	    "SELECT least("
	    "  coalesce(nullif(dictionary_page_offset, 0), data_page_offset),"
	    "  coalesce(nullif(index_page_offset, 0), data_page_offset),"
	    "  data_page_offset) AS start_offset,"
	    " total_compressed_size AS nbytes "
	    "FROM parquet_metadata('%s')",
	    escaped);

	Connection con(db);
	auto result = con.Query(query);
	if (result->HasError()) {
		throw InvalidInputException("Could not read Parquet metadata for '%s': %s", path,
		                            result->GetError());
	}

	vector<FileExtent> extents;
	for (auto &chunk_row : *result) {
		const auto start = chunk_row.GetValue<int64_t>(0);
		const auto nbytes = chunk_row.GetValue<int64_t>(1);
		if (start < 0 || nbytes <= 0) {
			continue;
		}
		extents.push_back(FileExtent {static_cast<idx_t>(start), static_cast<idx_t>(nbytes)});
	}
	return ParquetLayoutRegistry::Get().Register(path, std::move(extents));
}

} // namespace duckdb
