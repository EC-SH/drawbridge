#ifndef POOL_CONFIG_HPP
#define POOL_CONFIG_HPP

// PoolConfig.hpp — compile-time sizing of the SIP registrar's pre-allocated
// object pools (Issue #53 follow-up).
//
// pocket-dial pre-allocates every SipClient, Session and SipMessage up front so
// that the steady-state hot path performs ZERO heap allocations: the per-packet
// cost is bounded and there is no long-run heap fragmentation on the ESP32, which
// has no MMU and a finite, non-compacting heap. The price is that the entire
// budget is paid statically at boot regardless of load, so these caps ARE the
// device's hard concurrency limits. When a pool is exhausted the registrar
// degrades gracefully (REGISTER/INVITE answered with 503 Service Unavailable;
// the message pool falls back to a one-off heap allocation) — it never crashes.
//
// The three knobs below are plain object-like macros guarded by #ifndef so a
// build can override any of them from the command line, e.g.:
//
//     cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-DPOCKETDIAL_MAX_CLIENTS=64 -DPOCKETDIAL_MAX_SESSIONS=16"
//
// The defaults reproduce the historical hardcoded values exactly (32 clients,
// 8 sessions, 32 messages) so existing builds are bit-for-bit unchanged.
//
// Trade-off in one line: raise these for capacity, lower them to claw back RAM
// on a constrained SoftAP node. See docs/SCALING.md for per-tier recommendations,
// the per-object RAM cost, and what breaks if you 10x them.

// Maximum number of simultaneously REGISTERed SIP endpoints (extensions).
// Bounds the _clientPool. A 6th REGISTER beyond capacity (with no expired slot
// to evict) is answered 503 Service Unavailable.
#ifndef POCKETDIAL_MAX_CLIENTS
#define POCKETDIAL_MAX_CLIENTS 32
#endif

// Maximum number of concurrent call sessions (dialogs). Bounds the _sessionPool.
// RTP media flows peer-to-peer between phones, so a session costs the server only
// signalling/bookkeeping RAM — not bandwidth or DSP. An INVITE that cannot get a
// slot is answered 503 Service Unavailable.
#ifndef POCKETDIAL_MAX_SESSIONS
#define POCKETDIAL_MAX_SESSIONS 8
#endif

// Depth of the shared in-flight SipMessage scratch pool. Defaults to one slot per
// potential client, which comfortably covers steady-state REGISTER/OPTIONS churn.
// Broadcast/forking (the 999 all-page) transiently needs one message per target;
// if the pool is momentarily drained, getMessageFromPool() falls back to a single
// heap allocation rather than failing, so this value trades steady-state
// allocation-free operation against static footprint.
#ifndef POCKETDIAL_MSG_POOL
#define POCKETDIAL_MSG_POOL POCKETDIAL_MAX_CLIENTS
#endif

#endif
