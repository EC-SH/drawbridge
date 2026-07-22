#ifndef ADMIN_AUTH_HPP
#define ADMIN_AUTH_HPP

// AdminAuth: a small, self-contained, platform-portable admin credential and
// session manager for the HTTP dashboard.
//
// Why this exists: the dashboard's state-changing endpoints (/api/kill,
// /api/wifi/connect, /api/wifi/mode_ap, /api/factory-reset) were protected only
// by a same-origin/CSRF check. The device ships an OPEN WiFi AP, so any device
// that joins the AP could disconnect calls, rewrite WiFi credentials, switch
// modes, or factory-reset. AdminAuth adds a PIN-gated session layer on top of
// the existing same-origin check (defense in depth).
//
// Design constraints:
//   * Dependency-free beyond the C++17 standard library and the platform guards.
//   * Identical hashing on host and ESP (a self-contained, public-domain SHA-256
//     lives in AdminAuth.cpp) so a credential is portable and verifiable the same
//     way everywhere — and so the host build needs no external crypto dependency.
//   * Credential persistence: NVS namespace "storage" on ESP (keys admin_salt /
//     admin_hash); an in-process static on host (documented: host has no NVS).
//   * Thread-safety: all shared state is guarded by an internal std::mutex. The
//     HTTP server handles one client at a time per accept, but it detaches a
//     thread per connection, so concurrent access is possible.

#include <string>
#include <cstdint>

namespace AdminAuth
{
	// --- Tunables (documented in docs/THREAT_MODEL.md) ---
	constexpr size_t   kMinPinLength      = 4;        // reject PINs shorter than this
	constexpr size_t   kSessionTokenHex   = 32;       // >= 32 hex chars (128 bits)
	constexpr size_t   kMaxSessions       = 8;        // fixed-capacity session table
	constexpr uint64_t kSessionTtlMs      = 30ULL * 60ULL * 1000ULL;   // 30 min absolute expiry
	constexpr int      kMaxFailedAttempts = 5;        // consecutive failures before lockout
	constexpr uint64_t kLockoutMs         = 60ULL * 1000ULL;           // 60 s cooldown
	constexpr uint32_t kHashIterations    = 50000;    // PBKDF-style iterated SHA-256 rounds
	constexpr size_t   kMaxIpLockoutEntries = 64;     // bounded per-IP lockout table (issue #130)

	// Authentication channel for brute-force lockout accounting. Each channel keeps
	// an INDEPENDENT failure counter + cooldown so a flood of wrong PINs on one
	// surface cannot throttle the legitimate admin on another (issue #57: SSH PIN
	// spraying used to trip the single global lockout shared with HTTP login — a
	// cross-channel login-DoS). The credential itself is shared; only the
	// rate-limit state is per-channel. (Per-IP tracking WITHIN a channel remains a
	// documented follow-up — see docs/THREAT_MODEL.md D-3.)
	enum class Channel : int
	{
		Http = 0,   // HTTP dashboard /api/admin/login
		Ssh  = 1,   // SSH sysop terminal password auth (wolfSSH / littlessh)
		Dtmf = 2,   // in-call DTMF admin menu (*PIN#code)
		Count       // sentinel: size of the per-channel lockout table
	};

	// True iff an admin credential (salt + hash) is currently stored.
	bool isProvisioned();

	// Set/replace the admin PIN. Enforces kMinPinLength. Generates a fresh random
	// salt, computes a salted, iterated SHA-256 hash, and persists it (NVS on ESP,
	// in-memory on host). Returns false if the PIN is too short or persistence
	// fails. The caller (endpoint layer) decides *when* this is allowed.
	bool setPin(const std::string& pin);

	// Constant-time verification of a candidate PIN against the stored hash, scoped
	// to a brute-force `channel`. Honors that channel's lockout: returns false while
	// the channel is locked out (without even hashing). On a correct PIN, resets the
	// channel's failure counter. On a wrong PIN, increments it and may engage the
	// channel's lockout. Returns false if not provisioned. The default `Http` keeps
	// every existing caller (and the on-device DTMF/SSH paths pass their own channel)
	// source-compatible.
	//
	// `sourceIp` (issue #130): when non-zero AND channel == Http, lockout accounting
	// is scoped to THIS caller's source IP instead of the channel-wide counter, via a
	// bounded (kMaxIpLockoutEntries) table that evicts the least-recently-seen entry
	// on overflow. This stops one attacker IP from tripping a lockout that blocks the
	// legitimate admin's own IP (THREAT_MODEL.md D-3). Leaving `sourceIp` at its
	// default of 0 (or using a non-Http channel) keeps the original channel-wide
	// behavior — existing callers that don't have/need an IP are unaffected.
	bool verifyPin(const std::string& pin, Channel channel = Channel::Http, uint32_t sourceIp = 0);

	// True while the given channel's brute-force lockout is engaged (cooldown not yet
	// elapsed). Channels are independent (issue #57). See verifyPin() for `sourceIp`
	// (issue #130): non-zero + Http checks that IP's own lockout instead of the
	// channel-wide one.
	bool isLockedOut(Channel channel = Channel::Http, uint32_t sourceIp = 0);

	// Create a new server-side session and return its opaque random token
	// (kSessionTokenHex hex chars). Evicts the oldest/expired entry if the table
	// is full. Returns an empty string only on catastrophic RNG failure.
	std::string createSession();

	// True iff the token names a live (non-expired) session.
	bool validateSession(const std::string& token);

	// Destroy a session by token (logout). No-op if unknown.
	void destroySession(const std::string& token);

	// Wipe the stored credential (salt + hash) AND all live sessions, and reset the
	// lockout state. Used by factory-reset so the device returns to the
	// unprovisioned/open state.
	void clearCredential();

	// True iff the NVS key "admin_hash" in namespace "storage" is non-empty.
	// Intended for the boot provisioning gate in app_main() — called before
	// the RTOS scheduler has spawned real-time tasks, so it may block briefly
	// on NVS I/O without issue.
	// On non-ESP (host) builds this delegates to isProvisioned() so the host
	// unit tests exercise the same code path.
	bool credentialIsSet();
}

#endif // ADMIN_AUTH_HPP
