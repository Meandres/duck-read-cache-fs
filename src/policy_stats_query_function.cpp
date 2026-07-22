#include "policy_stats_query_function.hpp"

#include "cache_filesystem_config.hpp"
#include "cache_httpfs_instance_state.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/main/client_context.hpp"
#include "in_memory_cache_reader.hpp"
#include "policy_data_cache_storage.hpp"

namespace duckdb {

namespace {

struct PolicyStatsData : public GlobalTableFunctionState {
	vector<PolicyGroupStats> rows;
	idx_t offset = 0;
};

unique_ptr<FunctionData> PolicyStatsBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	ALWAYS_ASSERT(return_types.empty());
	ALWAYS_ASSERT(names.empty());

	auto add = [&return_types, &names](const char *name, LogicalTypeId type) {
		return_types.emplace_back(LogicalType {type});
		names.emplace_back(name);
	};
	add("policy_group", LogicalTypeId::VARCHAR);
	add("policy", LogicalTypeId::VARCHAR);
	add("capacity_bytes", LogicalTypeId::UBIGINT);
	add("used_bytes", LogicalTypeId::UBIGINT);
	add("entry_count", LogicalTypeId::UBIGINT);
	add("hits", LogicalTypeId::UBIGINT);
	add("misses", LogicalTypeId::UBIGINT);
	add("evictions", LogicalTypeId::UBIGINT);
	add("bytes_admitted", LogicalTypeId::UBIGINT);
	add("bytes_evicted", LogicalTypeId::UBIGINT);
	// The spread between these two is the point of variable-sized entries: if
	// they are equal, the run is still caching a fixed grid.
	add("min_entry_bytes", LogicalTypeId::UBIGINT);
	add("max_entry_bytes", LogicalTypeId::UBIGINT);
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> PolicyStatsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<PolicyStatsData>();
	auto &state = GetInstanceStateOrThrow(*context.db);
	for (auto *reader : state.cache_reader_manager.GetCacheReaders()) {
		if (reader == nullptr || reader->GetName() != *IN_MEM_CACHE_READER_NAME) {
			continue;
		}
		auto stats = reader->Cast<InMemoryCacheReader>().GetPolicyStats();
		for (auto &row : stats) {
			result->rows.push_back(std::move(row));
		}
	}
	return std::move(result);
}

void PolicyStatsTableFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<PolicyStatsData>();
	const idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, data.rows.size() - data.offset);
	if (count == 0) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		auto &row = data.rows[data.offset + i];
		idx_t col = 0;
		output.SetValue(col++, i, Value {row.group});
		output.SetValue(col++, i, Value {row.policy});
		output.SetValue(col++, i, Value::UBIGINT(row.capacity_bytes));
		output.SetValue(col++, i, Value::UBIGINT(row.used_bytes));
		output.SetValue(col++, i, Value::UBIGINT(row.entry_count));
		output.SetValue(col++, i, Value::UBIGINT(row.hits));
		output.SetValue(col++, i, Value::UBIGINT(row.misses));
		output.SetValue(col++, i, Value::UBIGINT(row.evictions));
		output.SetValue(col++, i, Value::UBIGINT(row.bytes_admitted));
		output.SetValue(col++, i, Value::UBIGINT(row.bytes_evicted));
		output.SetValue(col++, i, Value::UBIGINT(row.min_entry_bytes));
		output.SetValue(col++, i, Value::UBIGINT(row.max_entry_bytes));
	}
	data.offset += count;
	output.SetCardinality(count);
}

} // namespace

TableFunction GetPolicyStatsQueryFunc() {
	TableFunction policy_stats_func {"cache_httpfs_policy_stats", /*arguments=*/ {}, PolicyStatsTableFunc,
	                                 PolicyStatsBind, PolicyStatsInit};
	return policy_stats_func;
}

} // namespace duckdb
