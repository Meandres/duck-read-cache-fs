// Least Recently Used.
//
// The reference point: this is what the stock in-memory cache does, so an
// experiment that changes only the chunking axis should run this policy in
// order to hold eviction behaviour constant.

#pragma once

#include <list>
#include <unordered_map>

#include "policy/eviction_policy.hpp"

namespace duckdb {

class LruPolicy final : public EvictionPolicy {
public:
	void OnHit(const InMemCacheBlock &key) override {
		auto it = index.find(key);
		if (it == index.end()) {
			return;
		}
		// Most recent at the front.
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
		return "lru";
	}

private:
	std::list<InMemCacheBlock> order;
	std::unordered_map<InMemCacheBlock, std::list<InMemCacheBlock>::iterator, InMemCacheBlockHash,
	                   InMemCacheBlockEqual>
	    index;
};

} // namespace duckdb
