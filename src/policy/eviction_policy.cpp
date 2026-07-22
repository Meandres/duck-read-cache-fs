#include "policy/eviction_policy.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "policy/clock_policy.hpp"
#include "policy/fifo_policy.hpp"
#include "policy/lru_policy.hpp"
#include "policy/mru_policy.hpp"

namespace duckdb {

bool IsValidEvictionPolicy(const string &name) {
	return name == "lru" || name == "fifo" || name == "clock" || name == "mru";
}

unique_ptr<EvictionPolicy> CreateEvictionPolicy(const string &name) {
	if (name == "lru") {
		return make_uniq<LruPolicy>();
	}
	if (name == "fifo") {
		return make_uniq<FifoPolicy>();
	}
	if (name == "clock") {
		return make_uniq<ClockPolicy>();
	}
	if (name == "mru") {
		return make_uniq<MruPolicy>();
	}
	throw InvalidInputException("Unknown eviction policy '%s'; expected one of lru, fifo, clock, mru", name);
}

} // namespace duckdb
