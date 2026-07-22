// Make a local file behave like a remote object store.
//
// Every question this fork exists to answer -- fixed blocks vs column chunks,
// global vs per-file eviction -- turns on what a cache MISS costs. Against a
// local NVMe a miss is nearly free, so all designs look alike and the study
// measures nothing. Injecting a per-request delay restores the property that
// makes caching interesting, without needing an object store, a network
// namespace, or root.
//
// The model is the one an object store actually presents: a fixed per-request
// cost plus a transfer term.
//
//     delay_us = latency_us + bytes / (bandwidth_gbps * 1000)
//
// Requests sleep independently on the reader's worker threads, so concurrent
// subrequests overlap exactly as concurrent GETs would; the aggregate ceiling
// is therefore threads x bandwidth, with no global cap.
//
// Calibrated against MinIO over a 100 GbE link on this machine: a 512 KiB range
// GET cost 1.40 ms warm (2.25 ms random over a 25 GB object) and transferred at
// ~0.69 GB/s per request. So latency_us=1400, bandwidth_gbps=0.7 reproduces a
// fast same-datacenter store; 20000-40000 us is the range for real S3.
//
// Validated end-to-end at 1400/0.7 against that MinIO setup, same binary and
// query (full 16-column scan of TPC-H SF100 lineitem, 16 threads):
//
//                       real MinIO    simulated    error
//   uncached, cold         10.9 s       11.3 s     +3.7%
//   cached, cold           15.3 s       16.4 s     +7.0%
//   cached, warm           8.87 s       8.85 s     -0.3%
//
// Two known modelling limits, both measured:
//   * std::this_thread::sleep_for overshoots by ~100 us per request (timer
//     slack), so short latencies overcharge slightly -- conservative.
//   * The charge only makes sense on the request pattern the calibration was
//     measured under. DuckDB's reader coalesces reads for REMOTE files only,
//     which is why CacheFileSystem reports wrapped files as remote while
//     simulation is active; without that, local files see thousands of small
//     uncoalesced reads and the same knobs overcharge ~3.5x.

#pragma once

#include "duckdb/common/typedefs.hpp"

namespace duckdb {

struct InstanceConfig;

// Sleep for what fetching [bytes] would have cost. Returns immediately when
// simulation is off, which is the default.
void SimulateRemoteRead(const InstanceConfig &config, idx_t bytes);

} // namespace duckdb
