// CLOCK (second-chance FIFO).
//
// Approximates LRU with one reference bit per entry instead of a full ordering,
// which is what DuckDB's own buffer pool does to its eviction queue. Including
// it makes the comparison against the ExternalFileCache like-for-like.
//
// Entries are admitted with the reference bit CLEAR, so an entry touched once
// and never again is evicted on the hand's first pass. That is what keeps a
// single large scan from promoting its whole footprint, and it means CLOCK
// degenerates to FIFO exactly when there is no re-reference -- which is the
// behaviour we want to be able to point at in the write-up.

#pragma once

#include <list>
#include <unordered_map>

#include "policy/eviction_policy.hpp"

namespace duckdb {

class ClockPolicy final : public EvictionPolicy {
public:
	void OnHit(const InMemCacheBlock &key) override {
		auto it = index.find(key);
		if (it != index.end()) {
			it->second->referenced = true;
		}
	}

	void OnInsert(const InMemCacheBlock &key, idx_t size, double cost_us) override {
		if (index.find(key) != index.end()) {
			return;
		}
		// Insert behind the hand with the bit clear (see header comment).
		auto pos = ring.insert(hand, Entry {key, false});
		index[key] = pos;
	}

	void OnErase(const InMemCacheBlock &key) override {
		auto it = index.find(key);
		if (it == index.end()) {
			return;
		}
		if (hand == it->second) {
			Advance(hand);
		}
		ring.erase(it->second);
		index.erase(it);
		if (ring.empty()) {
			hand = ring.end();
		}
	}

	bool PopVictim(InMemCacheBlock &victim) override {
		if (ring.empty()) {
			return false;
		}
		if (hand == ring.end()) {
			hand = ring.begin();
		}
		// At most one full sweep clears every bit, so the second sweep must hit.
		for (idx_t guard = 0; guard < ring.size() * 2 + 1; guard++) {
			if (!hand->referenced) {
				victim = hand->key;
				index.erase(victim);
				hand = ring.erase(hand);
				if (hand == ring.end()) {
					hand = ring.begin();
				}
				return true;
			}
			hand->referenced = false;
			Advance(hand);
		}
		return false;
	}

	void Clear() override {
		ring.clear();
		index.clear();
		hand = ring.end();
	}

	string GetName() const override {
		return "clock";
	}

private:
	struct Entry {
		InMemCacheBlock key;
		bool referenced;
	};
	using Ring = std::list<Entry>;

	void Advance(Ring::iterator &it) {
		if (ring.empty()) {
			it = ring.end();
			return;
		}
		++it;
		if (it == ring.end()) {
			it = ring.begin();
		}
	}

	Ring ring;
	Ring::iterator hand = ring.end();
	std::unordered_map<InMemCacheBlock, Ring::iterator, InMemCacheBlockHash, InMemCacheBlockEqual> index;
};

} // namespace duckdb
