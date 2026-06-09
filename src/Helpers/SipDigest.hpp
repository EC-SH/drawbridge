#ifndef SIP_DIGEST_HPP
#define SIP_DIGEST_HPP

// SipDigest: self-contained SIP HTTP-Digest (RFC 2617, MD5 / qop=auth) primitives.
// NOTE: only the MD5 algorithm is implemented. RFC 8760 (SHA-256/SHA-512-256
// digest) is NOT supported — MD5 matches the installed-phone fleet; SHA-256 is a
// tracked hardening follow-up.
//
// Why this exists: the registrar at RequestsHandler::onRegister is fully open.
// STAGE 1 adds the digest-auth building blocks so a later stage can challenge a
// REGISTER with WWW-Authenticate, parse the client's Authorization, and verify
// the response against a stored HA1.
//
// Design constraints (mirrors AdminAuth):
//   * Dependency-free beyond the C++17 standard library and the platform guards.
//     A small, self-contained MD5 (RFC 1321 reference) lives in SipDigest.cpp so
//     the digest is identical on host and ESP and the host build needs NO external
//     crypto dependency. (IDF v6.0.1 only ships mbedtls/private/md5.h, so vendoring
//     a portable MD5 is both more portable AND avoids the private-header churn.)
//   * Pure functions + a nonce helper. No NVS, no SIP, no sockets here — the
//     secret store (SipSecretStore) and the SIP wiring live elsewhere.
//   * Constant-time response comparison so verification does not leak how many
//     leading hex digits matched.
//
// Algorithm (RFC 2617, qop="auth"):
//   HA1      = MD5(username:realm:password)              <- computeHa1
//   HA2      = MD5(method:digestURI)
//   response = MD5(HA1:nonce:nc:cnonce:qop:HA2)          <- computeResponse
//
// The realm is fixed to "pocketdial" by the caller (not hard-coded here).

#include <string>
#include <cstdint>

namespace SipDigest
{
	// Parsed fields of an inbound `Authorization: Digest ...` header value.
	// Only the digest parameters relevant to qop="auth" verification are kept.
	// Missing parameters are left as empty strings.
	struct DigestAuth
	{
		std::string username;
		std::string realm;
		std::string nonce;
		std::string uri;        // the "uri" digest-uri parameter
		std::string response;   // 32 hex chars
		std::string qop;        // "auth" (or empty for legacy RFC 2069)
		std::string nc;         // nonce-count, 8 hex chars (e.g. "00000001")
		std::string cnonce;     // client nonce
		std::string algorithm;  // "MD5" (or empty -> defaults to MD5)
		std::string opaque;     // echoed back if the server issued one
	};

	// --- MD5 helper (exposed because the secret store and tests want it) ---
	// Returns the lowercase 32-char hex MD5 of `input`.
	std::string md5Hex(const std::string& input);

	// --- Header emit / parse ---------------------------------------------

	// Build a `WWW-Authenticate` HEADER VALUE (without the "WWW-Authenticate: "
	// name) suitable for a 401 challenge:
	//   Digest realm="<realm>", nonce="<nonce>", qop="auth", algorithm=MD5
	// When `stale` is true, appends `, stale=true` so a client that used an
	// expired nonce knows to retry with fresh credentials rather than re-prompt.
	std::string buildWwwAuthenticate(const std::string& realm,
	                                 const std::string& nonce,
	                                 bool stale = false);

	// Parse an `Authorization` HEADER VALUE. Accepts either the full header line
	// ("Authorization: Digest ...") or just the value ("Digest ..."). Tolerates
	// arbitrary whitespace, optional quoting, and parameter reordering. Returns
	// true iff the value parsed as a Digest credential with a non-empty
	// username/response (the minimum needed to attempt verification).
	bool parseAuthorization(const std::string& authHeaderValue, DigestAuth& out);

	// --- Digest computation ----------------------------------------------

	// HA1 = MD5(ext:realm:secret). This is what the secret store persists.
	std::string computeHa1(const std::string& ext,
	                       const std::string& realm,
	                       const std::string& secret);

	// response = MD5(HA1:nonce:nc:cnonce:qop:HA2), HA2 = MD5(method:uri).
	// When `qop` is empty, falls back to the legacy RFC 2069 form
	// response = MD5(HA1:nonce:HA2) so older UACs still verify.
	std::string computeResponse(const std::string& ha1,
	                            const std::string& method,
	                            const std::string& uri,
	                            const std::string& nonce,
	                            const std::string& nc,
	                            const std::string& cnonce,
	                            const std::string& qop);

	// Constant-time verify of `auth.response` against the locally-recomputed
	// response built from `ha1` and the request `method`. All digest inputs
	// (uri/nonce/nc/cnonce/qop) come from the parsed `auth`. Returns false on a
	// length mismatch or any differing byte (no short-circuit).
	bool verify(const DigestAuth& auth,
	            const std::string& ha1,
	            const std::string& method);

	// --- Nonce helper -----------------------------------------------------
	//
	// Nonces are stateless and self-validating: a nonce embeds a creation
	// timestamp and an HMAC-style keyed MD5 tag over that timestamp using a
	// process-lifetime server secret. No server-side nonce table is needed, and
	// a tampered or expired nonce is detected on the next request.
	//
	// Wire form: lowercase hex of "<timestampMs>" + ":" + MD5(secret:timestampMs),
	// joined as "<hex(timestampMs)>.<tagHex>" (the '.' is not a hex char so it is
	// an unambiguous separator).

	// Default nonce lifetime: 5 minutes. Long enough for a phone to compute and
	// resend its credentials, short enough to bound replay.
	constexpr uint64_t kNonceTtlMs = 5ULL * 60ULL * 1000ULL;

	// Generate a fresh nonce bound to "now" and signed with the server secret.
	std::string generateNonce();

	// Validate a nonce's integrity AND freshness. Returns true iff the tag verifies
	// (it is one WE issued) AND the nonce is within `ttlMs`. If `expiredOut` is
	// non-null it is set true when the tag verified but the nonce is past `ttlMs`
	// (i.e. stale → answer 401 with stale=true), and false otherwise. Returns false
	// for a forged/corrupt tag OR an expired nonce.
	bool validateNonce(const std::string& nonce,
	                   bool* expiredOut = nullptr,
	                   uint64_t ttlMs = kNonceTtlMs);

	// Convenience: a nonce is "stale" iff its tag verifies (it is one WE issued)
	// but it is past `ttlMs`. A stale nonce -> answer 401 with stale=true so the
	// client silently retries. A forged/garbage nonce is NOT stale (returns false)
	// -> treat as a fresh challenge / hard reject.
	bool isStale(const std::string& nonce, uint64_t ttlMs = kNonceTtlMs);
}

#endif // SIP_DIGEST_HPP
