// Most Recently Used.
//
// Not a serious candidate for deployment -- it is here as a HARNESS CHECK.
//
// On a cyclic scan whose working set is a little larger than the cache, LRU,
// FIFO and CLOCK all evict exactly the block that is about to be read next and
// collapse to a near-zero hit ratio, while MRU keeps most of the cache
// resident. So MRU should beat the others by a wide margin on that workload and
// lose everywhere else.
//
// If a run shows MRU roughly tied with the rest, the harness is not exercising
// replacement at all -- the cache is oversized, the keys are not being reused,
// or reads are bypassing the cache -- and every other policy number from that
// run is meaningless. Check this before believing any policy comparison.

#pragma once

#include <list>
#include <unordered_map>

#include "policy/eviction_policy.hpp"

namespace duckdb {

class MruPolicy final : public EvictionPolicy {
public:
	void OnHit(const InMemCacheBlock &key) override {
		auto it = index.find(key);
		if (it == index.end()) {
			return;
		}
		order.splice(order.begin(), order, it->second);
	}

	void OnInsert(const InMemCacheBlock &key, idx_t size, double cost_us) override {
		auto it = index.find(key);
		if (it != index.end()) {
			order.splice(order.begin(), order, it->second);
			return;
		}
		order.push_front(key);
		index[key] = order.begin();
	}

	void OnErase(const InMemCacheBlock &key) override {
		auto it = index.find(key);
		if (it == index.end()) {
			return;
		}
		order.erase(it->second);
		index.erase(it);
	}

	// The only difference from LRU: evict the front (most recent), not the back.
	bool PopVictim(InMemCacheBlock &victim) override {
		if (order.empty()) {
			return false;
		}
		victim = order.front();
		index.erase(victim);
		order.pop_front();
		return true;
	}

	void Clear() override {
		order.clear();
		index.clear();
	}

	string GetName() const override {
		return "mru";
	}

private:
	std::list<InMemCacheBlock> order;
	std::unordered_map<InMemCacheBlock, std::list<InMemCacheBlock>::iterator, InMemCacheBlockHash,
	                   InMemCacheBlockEqual>
	    index;
};

} // namespace duckdb
