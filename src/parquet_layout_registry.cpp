#include "parquet_layout_registry.hpp"

#include <algorithm>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
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

	// The planner relies on extents being disjoint and ascending; an overlap would let the same bytes be cached under
	// two different keys.
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

LayoutRegion ParquetLayoutRegistry::Locate(const string &path, idx_t offset, idx_t &start, idx_t &end) const {
	std::lock_guard<std::mutex> guard(lock);
	auto it = layouts.find(path);
	if (it == layouts.end()) {
		return LayoutRegion::kNoLayout;
	}
	const auto &extents = it->second;

	// First extent starting after [offset]; the one before it is the only candidate to contain [offset].
	auto next = std::upper_bound(extents.begin(), extents.end(), offset,
	                             [](idx_t value, const FileExtent &e) { return value < e.offset; });
	if (next != extents.begin()) {
		auto prev = std::prev(next);
		if (offset < prev->End()) {
			start = prev->offset;
			end = prev->End();
			return LayoutRegion::kExtent;
		}
		start = prev->End();
	} else {
		start = 0;
	}
	end = next != extents.end() ? next->offset : NumericLimits<idx_t>::Maximum();
	return LayoutRegion::kGap;
}

idx_t ParquetLayoutRegistry::FileCount() const {
	std::lock_guard<std::mutex> guard(lock);
	return layouts.size();
}

void ParquetLayoutRegistry::Clear() {
	std::lock_guard<std::mutex> guard(lock);
	layouts.clear();
}

idx_t RegisterParquetLayoutFromFooter(DatabaseInstance &db, const string &path) {
	// Mirrors duckdb::ColumnReader::FileOffset: a column chunk starts at the earliest of its dictionary, index and data
	// page offsets. The unset ones come back as NULL or 0, hence the nullif/coalesce.
	const string escaped = StringUtil::Replace(path, "'", "''");
	const string query = StringUtil::Format("SELECT least("
	                                        "  coalesce(nullif(dictionary_page_offset, 0), data_page_offset),"
	                                        "  coalesce(nullif(index_page_offset, 0), data_page_offset),"
	                                        "  data_page_offset) AS start_offset,"
	                                        " total_compressed_size AS nbytes "
	                                        "FROM parquet_metadata('%s')",
	                                        escaped);

	Connection con(db);
	auto result = con.Query(query);
	if (result->HasError()) {
		throw InvalidInputException("Could not read Parquet metadata for '%s': %s", path, result->GetError());
	}

	vector<FileExtent> extents;
	for (auto &row : *result) {
		const auto start = row.GetValue<int64_t>(0);
		const auto nbytes = row.GetValue<int64_t>(1);
		if (start < 0 || nbytes <= 0) {
			continue;
		}
		extents.push_back(FileExtent {static_cast<idx_t>(start), static_cast<idx_t>(nbytes)});
	}
	return ParquetLayoutRegistry::Get().Register(path, std::move(extents));
}

} // namespace duckdb
