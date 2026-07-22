#ifndef SYSLOG_HPP
#define SYSLOG_HPP

// Syslog: RFC 5424 UDP syslog sink (issue #129), alongside the existing serial
// log — not a replacement for it. Disabled (a no-op) until a destination is
// configured, either via loadFromNvs() (NVS namespace "pbxcfg", key
// "syslog_host" + optional numeric "syslog_port", default 514) or a direct
// configure() call (what host tests use).
//
// Zero-alloc hot path: send() formats into a fixed stack buffer and does a
// single fire-and-forget UDP send() on a persistent, pre-connected socket — no
// retry, no queue, no TCP/TLS. Reliable delivery is the aggregator's concern,
// per the issue. `host` must be a numeric IPv4 address (no DNS resolution) to
// keep this dependency-free and non-blocking.

#include <string>
#include <cstdint>

namespace Syslog
{
	// RFC 5424 severities actually used here (a small, deliberate subset —
	// this sink logs call/registration EVENTS, not general diagnostic noise).
	enum class Severity : int
	{
		Error   = 3,
		Warning = 4,
		Info    = 6,
	};

	// Reads "syslog_host"/"syslog_port" from NVS ns "pbxcfg" at boot. No-op on
	// host builds (host has no NVS) and if the key is absent/empty — the sink
	// simply stays unconfigured, exactly like a fresh ESP unit that never had
	// the feature turned on.
	void loadFromNvs();

	// (Re)points the sink at host:port. An empty host disables the sink
	// (isConfigured() becomes false, send() becomes a no-op) and closes any
	// existing socket. Returns false if `host` isn't a parseable numeric IPv4
	// address or socket setup failed; the sink is left/returned to disabled.
	bool configure(const std::string& host, uint16_t port = 514);

	bool isConfigured();

	// Emits one RFC 5424 datagram: "<PRI>1 - - drawbridge APP-NAME - - MSG"
	// (TIMESTAMP/HOSTNAME/PROCID/MSGID/STRUCTURED-DATA are the RFC 5424 NILVALUE
	// "-" — this device has no guaranteed wall clock without NTP). `appName`
	// becomes the APP-NAME field (e.g. "pbx-call", "pbx-register"); `msg` is
	// free-text, conventionally "key=value" pairs for the aggregator to parse.
	// No-op if not configured.
	void send(Severity severity, const std::string& appName, const std::string& msg);
}

#endif // SYSLOG_HPP
