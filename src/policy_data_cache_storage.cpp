#include "policy_data_cache_storage.hpp"

#include <algorithm>
#include <utility>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

namespace {

// Parse `pattern=policy[:size]` entries separated by ';'.
// [size] is either an absolute byte count or a percentage of the total budget.
struct SpecEntry {
	string pattern;
	string policy;
	idx_t bytes = 0;      // 0 with percent == 0 means "share the leftover"
	double percent = 0.0; // of the total budget
};

// StringUtil::Trim mutates in place and returns void.
string Trimmed(string s) {
	StringUtil::Trim(s);
	return s;
}

vector<SpecEntry> ParseFilePolicySpec(const string &spec, idx_t max_bytes) {
	vector<SpecEntry> out;
	if (spec.empty()) {
		return out;
	}
	for (auto &part : StringUtil::Split(spec, ';')) {
		auto trimmed = Trimmed(part);
		if (trimmed.empty()) {
			continue;
		}
		auto eq = trimmed.find('=');
		if (eq == string::npos) {
			throw InvalidInputException("Malformed file policy entry '%s'; expected pattern=policy[:bytes]", trimmed);
		}
		SpecEntry entry;
		entry.pattern = Trimmed(trimmed.substr(0, eq));
		auto rhs = Trimmed(trimmed.substr(eq + 1));
		auto colon = rhs.find(':');
		if (colon == string::npos) {
			entry.policy = rhs;
		} else {
			entry.policy = Trimmed(rhs.substr(0, colon));
			auto size_str = Trimmed(rhs.substr(colon + 1));
			const bool is_percent = !size_str.empty() && size_str.back() == '%';
			try {
				if (is_percent) {
					entry.percent = std::stod(size_str.substr(0, size_str.size() - 1));
					if (entry.percent <= 0.0 || entry.percent > 100.0) {
						throw InvalidInputException("Percentage must be in (0, 100] in file policy entry '%s'",
						                            trimmed);
					}
					entry.bytes = static_cast<idx_t>(static_cast<double>(max_bytes) * entry.percent / 100.0);
				} else {
					entry.bytes = static_cast<idx_t>(std::stoull(size_str));
				}
			} catch (InvalidInputException &) {
				throw;
			} catch (std::exception &) {
				throw InvalidInputException("Malformed size '%s' in file policy entry '%s'; expected a byte count "
				                            "or a percentage such as 25%%",
				                            size_str, trimmed);
			}
			if (entry.bytes == 0) {
				throw InvalidInputException(
				    "Size '%s' in file policy entry '%s' resolves to 0 bytes; is the total cache size set?", size_str,
				    trimmed);
			}
		}
		if (entry.pattern.empty()) {
			throw InvalidInputException("Empty pattern in file policy entry '%s'", trimmed);
		}
		if (!IsValidEvictionPolicy(entry.policy)) {
			throw InvalidInputException("Unknown eviction policy '%s' in file policy entry '%s'", entry.policy,
			                            trimmed);
		}
		out.push_back(std::move(entry));
	}
	return out;
}

} // namespace

PolicyDataCacheStorage::PolicyDataCacheStorage(idx_t max_bytes_p, uint64_t timeout_millisec_p,
                                               const string &default_policy, const string &file_policy_spec,
                                               bool per_file_scope)
    : max_bytes(max_bytes_p), timeout_millisec(timeout_millisec_p) {
	if (!IsValidEvictionPolicy(default_policy)) {
		throw InvalidInputException("Unknown eviction policy '%s'", default_policy);
	}

	auto spec = per_file_scope ? ParseFilePolicySpec(file_policy_spec, max_bytes_p) : vector<SpecEntry>();

	// Explicitly-sized groups take their budget off the top; everything else
	// splits what is left, the catch-all group included.
	idx_t explicit_bytes = 0;
	idx_t unsized_groups = 1; // the catch-all
	for (auto &entry : spec) {
		if (entry.bytes > 0) {
			explicit_bytes += entry.bytes;
		} else {
			unsized_groups++;
		}
	}
	if (max_bytes > 0 && explicit_bytes > max_bytes) {
		throw InvalidInputException(
		    "File policy budgets total %llu bytes, which exceeds the cache budget of %llu bytes",
		    static_cast<unsigned long long>(explicit_bytes), static_cast<unsigned long long>(max_bytes));
	}
	const idx_t share = (max_bytes == 0) ? 0 : (max_bytes - explicit_bytes) / unsized_groups;

	const bool bounded = max_bytes > 0;
	for (auto &entry : spec) {
		auto group = make_uniq<Group>();
		group->name = entry.pattern;
		group->pattern = entry.pattern;
		group->policy = CreateEvictionPolicy(entry.policy);
		group->bounded = bounded;
		group->capacity_bytes = entry.bytes > 0 ? entry.bytes : share;
		groups.push_back(std::move(group));
	}

	// The catch-all must be last: GroupFor takes the first pattern match.
	auto fallback = make_uniq<Group>();
	fallback->name = spec.empty() ? "global" : "default";
	fallback->policy = CreateEvictionPolicy(default_policy);
	fallback->bounded = bounded;
	fallback->capacity_bytes = share;
	groups.push_back(std::move(fallback));
}

PolicyDataCacheStorage::Group &PolicyDataCacheStorage::GroupFor(const string &fname) {
	auto it = group_by_file.find(fname);
	if (it != group_by_file.end()) {
		return *it->second;
	}
	Group *chosen = groups.back().get();
	for (auto &group : groups) {
		if (!group->pattern.empty() && fname.find(group->pattern) != string::npos) {
			chosen = group.get();
			break;
		}
	}
	group_by_file[fname] = chosen;
	return *chosen;
}

bool PolicyDataCacheStorage::Expired(const Entry &entry) const {
	if (timeout_millisec == 0) {
		return false;
	}
	const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
	                                                                      entry.inserted_at)
	                     .count();
	return static_cast<uint64_t>(age) >= timeout_millisec;
}

void PolicyDataCacheStorage::EvictToFit(Group &group, idx_t needed) {
	if (!group.bounded) {
		return;
	}
	InMemCacheBlock victim {"", 0, 0};
	while (group.used_bytes + needed > group.capacity_bytes) {
		if (!group.policy->PopVictim(victim)) {
			// The policy is empty but the accounting still says we are over
			// budget: nothing more can be freed, so admit and move on rather
			// than spin.
			return;
		}
		auto it = group.entries.find(victim);
		if (it == group.entries.end()) {
			continue;
		}
		group.used_bytes -= it->second.bytes;
		group.bytes_evicted += it->second.bytes;
		group.evictions++;
		group.entries.erase(it);
	}
}

void PolicyDataCacheStorage::Put(InMemCacheBlock key, PageAlignedDataChunk chunk, string version_tag) {
	// The allocation, not the valid prefix, is what occupies memory.
	const idx_t bytes = chunk.capacity > 0 ? chunk.capacity : chunk.length;

	std::lock_guard<std::mutex> guard(lock);
	auto &group = GroupFor(key.fname);

	if (!group.policy->Admit(bytes, /*cost_us=*/0.0)) {
		return;
	}
	// An entry larger than the whole group can never be held. This also covers
	// a group whose explicit budget worked out to zero: it caches nothing.
	if (group.bounded && bytes > group.capacity_bytes) {
		return;
	}

	auto existing = group.entries.find(key);
	if (existing != group.entries.end()) {
		group.used_bytes -= existing->second.bytes;
		group.entries.erase(existing);
		group.policy->OnErase(key);
	}

	EvictToFit(group, bytes);

	Entry entry;
	entry.data = make_shared_ptr<InMemCacheDataEntry>();
	entry.data->data = std::move(chunk);
	entry.data->version_tag = std::move(version_tag);
	entry.bytes = bytes;
	entry.inserted_at = std::chrono::steady_clock::now();

	group.used_bytes += bytes;
	group.bytes_admitted += bytes;
	group.min_entry_bytes = group.min_entry_bytes == 0 ? bytes : MinValue<idx_t>(group.min_entry_bytes, bytes);
	group.max_entry_bytes = MaxValue<idx_t>(group.max_entry_bytes, bytes);
	group.entries[key] = std::move(entry);
	group.policy->OnInsert(key, bytes, /*cost_us=*/0.0);
}

optional<PinnedBlock> PolicyDataCacheStorage::Get(const InMemCacheBlock &key, const string &expected_version_tag) {
	std::lock_guard<std::mutex> guard(lock);
	auto &group = GroupFor(key.fname);

	auto it = group.entries.find(key);
	if (it == group.entries.end()) {
		group.misses++;
		return nullopt;
	}
	if (Expired(it->second) || !PinnedBlock::ValidateVersionTag(it->second.data->version_tag, expected_version_tag)) {
		group.used_bytes -= it->second.bytes;
		group.entries.erase(it);
		group.policy->OnErase(key);
		group.misses++;
		return nullopt;
	}

	group.hits++;
	group.policy->OnHit(key);
	// The shared_ptr keeps the payload alive even if a concurrent Put evicts it.
	auto keep_alive = it->second.data;
	const PageAlignedDataChunk *chunk = &keep_alive->data;
	return PinnedBlock(std::move(keep_alive), chunk);
}

bool PolicyDataCacheStorage::Delete(const InMemCacheBlock &key) {
	std::lock_guard<std::mutex> guard(lock);
	auto &group = GroupFor(key.fname);
	auto it = group.entries.find(key);
	if (it == group.entries.end()) {
		return false;
	}
	group.used_bytes -= it->second.bytes;
	group.entries.erase(it);
	group.policy->OnErase(key);
	return true;
}

void PolicyDataCacheStorage::Clear() {
	std::lock_guard<std::mutex> guard(lock);
	for (auto &group : groups) {
		group->entries.clear();
		group->policy->Clear();
		group->used_bytes = 0;
	}
}

void PolicyDataCacheStorage::Clear(const InMemCacheBlock &start_key,
                                   std::function<bool(const InMemCacheBlock &)> filter) {
	std::lock_guard<std::mutex> guard(lock);
	// The interface documents an ordered scan from [start_key]; this storage is
	// hash-based, so apply the filter to every entry instead. Same outcome, and
	// invalidation is not on the hot path.
	for (auto &group : groups) {
		for (auto it = group->entries.begin(); it != group->entries.end();) {
			if (filter(it->first)) {
				group->used_bytes -= it->second.bytes;
				group->policy->OnErase(it->first);
				it = group->entries.erase(it);
			} else {
				++it;
			}
		}
	}
}

vector<InMemCacheBlock> PolicyDataCacheStorage::Keys() const {
	std::lock_guard<std::mutex> guard(lock);
	vector<InMemCacheBlock> keys;
	for (auto &group : groups) {
		for (auto &kv : group->entries) {
			keys.push_back(kv.first);
		}
	}
	return keys;
}

vector<std::pair<InMemCacheBlock, shared_ptr<InMemCacheDataEntry>>> PolicyDataCacheStorage::Take() {
	std::lock_guard<std::mutex> guard(lock);
	vector<std::pair<InMemCacheBlock, shared_ptr<InMemCacheDataEntry>>> taken;
	for (auto &group : groups) {
		for (auto &kv : group->entries) {
			taken.emplace_back(kv.first, kv.second.data);
		}
		group->entries.clear();
		group->policy->Clear();
		group->used_bytes = 0;
	}
	return taken;
}

vector<PolicyGroupStats> PolicyDataCacheStorage::GetStats() const {
	std::lock_guard<std::mutex> guard(lock);
	vector<PolicyGroupStats> stats;
	for (auto &group : groups) {
		PolicyGroupStats row;
		row.group = group->name;
		row.policy = group->policy->GetName();
		row.capacity_bytes = group->capacity_bytes;
		row.used_bytes = group->used_bytes;
		row.entry_count = group->entries.size();
		row.hits = group->hits;
		row.misses = group->misses;
		row.evictions = group->evictions;
		row.bytes_admitted = group->bytes_admitted;
		row.bytes_evicted = group->bytes_evicted;
		row.min_entry_bytes = group->min_entry_bytes;
		row.max_entry_bytes = group->max_entry_bytes;
		stats.push_back(std::move(row));
	}
	return stats;
}

} // namespace duckdb
