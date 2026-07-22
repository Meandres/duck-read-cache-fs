// In-memory data cache bounded by BYTES, with a pluggable eviction policy and
// optional per-file policy groups.
//
// Differs from ExtensionBoundedDataCacheStorage in the two ways the study needs:
//
//   * bounded by bytes, not entry count. Once cache entries follow Parquet
//     column chunks their sizes span orders of magnitude, and an entry-count
//     bound stops corresponding to any amount of memory.
//   * the replacement decision is a policy object rather than hard-wired LRU,
//     and the cache can be partitioned so different files evict independently.
//
// Group model. In `global` scope there is one group holding the whole budget.
// In `per_file` scope each pattern in the policy spec gets its own group with
// its own budget and its own policy instance, so a scan-heavy file cannot evict
// a small hot file's entries -- that is the "one policy per file vs one global
// policy" question, made measurable.

#pragma once

#include <chrono>
#include <mutex>
#include <unordered_map>

#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "in_memory_data_cache_storage.hpp"
#include "policy/eviction_policy.hpp"

namespace duckdb {

// One row of `cache_httpfs_policy_stats()`.
struct PolicyGroupStats {
	string group;
	string policy;
	idx_t capacity_bytes = 0;
	idx_t used_bytes = 0;
	idx_t entry_count = 0;
	idx_t hits = 0;
	idx_t misses = 0;
	idx_t evictions = 0;
	idx_t bytes_admitted = 0;
	idx_t bytes_evicted = 0;
	idx_t min_entry_bytes = 0;
	idx_t max_entry_bytes = 0;
};

class PolicyDataCacheStorage final : public InMemoryDataCacheStorage {
public:
	// @param max_bytes: total budget across all groups. 0 means unbounded.
	// @param timeout_millisec: per-entry timeout. 0 means no timeout.
	// @param default_policy: policy for the catch-all group.
	// @param file_policy_spec: ';'-separated `pattern=policy[:bytes]` entries.
	//        [pattern] matches as a substring of the file path.
	// @param per_file_scope: when false the spec is ignored and everything
	//        shares one group, which is the control condition for the per-file
	//        experiment.
	PolicyDataCacheStorage(idx_t max_bytes, uint64_t timeout_millisec, const string &default_policy,
	                       const string &file_policy_spec, bool per_file_scope);

	PolicyDataCacheStorage(const PolicyDataCacheStorage &) = delete;
	PolicyDataCacheStorage &operator=(const PolicyDataCacheStorage &) = delete;

	~PolicyDataCacheStorage() override = default;

	void Put(InMemCacheBlock key, PageAlignedDataChunk chunk, string version_tag) override;
	optional<PinnedBlock> Get(const InMemCacheBlock &key, const string &expected_version_tag) override;
	bool Delete(const InMemCacheBlock &key) override;
	void Clear() override;
	void Clear(const InMemCacheBlock &start_key, std::function<bool(const InMemCacheBlock &)> filter) override;
	vector<InMemCacheBlock> Keys() const override;
	vector<std::pair<InMemCacheBlock, shared_ptr<InMemCacheDataEntry>>> Take() override;

	vector<PolicyGroupStats> GetStats() const;

private:
	struct Entry {
		shared_ptr<InMemCacheDataEntry> data;
		idx_t bytes = 0;
		std::chrono::steady_clock::time_point inserted_at;
	};

	struct Group {
		string name;
		// Empty for the catch-all group; otherwise the path substring to match.
		string pattern;
		unique_ptr<EvictionPolicy> policy;
		// [capacity_bytes] is only meaningful when [bounded]. The two are kept
		// apart so that a group whose explicit budget works out to zero caches
		// nothing, rather than reading as "0 == no limit" and growing freely.
		bool bounded = false;
		idx_t capacity_bytes = 0;
		idx_t used_bytes = 0;
		std::unordered_map<InMemCacheBlock, Entry, InMemCacheBlockHash, InMemCacheBlockEqual> entries;

		idx_t hits = 0;
		idx_t misses = 0;
		idx_t evictions = 0;
		idx_t bytes_admitted = 0;
		idx_t bytes_evicted = 0;
		idx_t min_entry_bytes = 0;
		idx_t max_entry_bytes = 0;
	};

	// Caller must hold [lock].
	Group &GroupFor(const string &fname);
	void EvictToFit(Group &group, idx_t needed);
	bool Expired(const Entry &entry) const;

	mutable std::mutex lock;
	vector<unique_ptr<Group>> groups;
	// Resolved group per file path, so pattern matching happens once per file.
	std::unordered_map<string, Group *> group_by_file;
	idx_t max_bytes;
	uint64_t timeout_millisec;
};

} // namespace duckdb
