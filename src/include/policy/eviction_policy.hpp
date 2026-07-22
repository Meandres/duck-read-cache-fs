// Pluggable eviction policy for the in-memory data block cache.
//
// The stock in-memory cache is an LRU bounded by ENTRY COUNT, which is only
// meaningful while every entry is one fixed-size block. Once cache entries
// follow Parquet column chunks they vary by several orders of magnitude (45 B
// to 5 MiB in TPC-H SF100), so the cache has to be bounded by BYTES and the
// replacement decision has to be able to take size into account.
//
// A policy owns the ordering metadata only; the bytes live in the storage that
// drives it. `OnInsert` therefore receives the entry size, and victims are
// pulled one at a time until enough room has been freed.

#pragma once

#include <cstddef>
#include <functional>

#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "in_mem_cache_block.hpp"

namespace duckdb {

// The stock key carries only ordering comparators; the policies want hashing.
struct InMemCacheBlockHash {
	std::size_t operator()(const InMemCacheBlock &b) const {
		std::size_t h = std::hash<string>()(b.fname);
		auto mix = [&h](idx_t v) {
			h ^= std::hash<idx_t>()(v) + 0x9e3779b9U + (h << 6) + (h >> 2);
		};
		mix(b.start_off);
		mix(b.blk_size);
		return h;
	}
};

class EvictionPolicy {
public:
	virtual ~EvictionPolicy() = default;

	// A cached entry was read.
	virtual void OnHit(const InMemCacheBlock &key) = 0;

	// An entry was admitted. [size] is the entry's byte size; [cost_us] is what
	// fetching it cost, for policies that weigh cost against size. Both are
	// passed even by policies that ignore them, so a size-aware policy can be
	// added without touching this interface or its callers.
	virtual void OnInsert(const InMemCacheBlock &key, idx_t size, double cost_us) = 0;

	// An entry left the cache for a reason other than eviction (explicit
	// invalidation, timeout, block-size remap).
	virtual void OnErase(const InMemCacheBlock &key) = 0;

	// Choose the next entry to evict. Returns false when the policy holds
	// nothing, which the caller must treat as "cannot free more".
	virtual bool PopVictim(InMemCacheBlock &victim) = 0;

	// Admission control. Returning false caches nothing and serves the read
	// straight through -- the hook a scan-resistant policy needs.
	virtual bool Admit(idx_t size, double cost_us) {
		return true;
	}

	virtual void Clear() = 0;

	virtual string GetName() const = 0;
};

// Build a policy by name. Throws on an unknown name so a typo in a benchmark
// script fails loudly instead of silently falling back to LRU.
unique_ptr<EvictionPolicy> CreateEvictionPolicy(const string &name);

// Whether [name] is a policy this build knows about.
bool IsValidEvictionPolicy(const string &name);

} // namespace duckdb
