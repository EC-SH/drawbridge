#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Start a minimal, client-only SNTP time sync from NIST. Smallest sane exposure surface:
//   - SNTP CLIENT (poll mode) only — no listening socket, no inbound port.
//   - a single hardcoded NIST server; DHCP-advertised time servers are IGNORED.
//   - the UDP socket is opened only for the brief sync, then released (no persistent
//     footprint — matters on this socket-constrained box).
//   - a plausibility bound reverts an absurd reply so a bogus packet can't smash the clock.
// Sets the system wall clock (UTC); callers of time()/gettimeofday() (CDRs, logs) then get
// real time. Does NOT touch the monotonic esp_timer the anchor token logic relies on.
// Call once, after the network has an IP. Spawns one low-priority task (syncs now, then
// re-syncs periodically to correct RTC drift); never blocks the caller.
void ntp_time_start(void);

#ifdef __cplusplus
}
#endif
