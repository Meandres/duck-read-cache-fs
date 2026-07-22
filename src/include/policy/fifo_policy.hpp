// First In First Out.
//
// Evicts in insertion order and ignores reuse entirely. Useful as the floor
// against which any recency- or size-aware policy has to justify itself: if a
// workload shows no gap between FIFO and LRU, its reuse pattern is not one that
// replacement policy can exploit.

#pragma once

#include <list>
#include <unordered_map>

#include "policy/eviction_policy.hpp"

namespace duckdb {

class FifoPolicy final : public EvictionPolicy {
public:
	void OnHit(const InMemCacheBlock &key) override {
		// Deliberately empty: a hit does not change eviction order under FIFO.
	}

	void OnInsert(const InMemCacheBlock &key, idx_t size, double cost_us) override {
		if (index.find(key) != index.end()) {
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

	bool PopVictim(InMemCacheBlock &victim) override {
		if (order.empty()) {
			return false;
		}
		victim = order.back();
		index.erase(victim);
		order.pop_back();
		return true;
	}

	void Clear() override {
		order.clear();
		index.clear();
	}

	string GetName() const override {
		return "fifo";
	}

private:
	std::list<InMemCacheBlock> order;
	std::unordered_map<InMemCacheBlock, std::list<InMemCacheBlock>::iterator, InMemCacheBlockHash,
	                   InMemCacheBlockEqual>
	    index;
};

} // namespace duckdb
