// AdminAuthLockout_test.cpp — issue #57: the admin-PIN brute-force lockout must be
// PER-CHANNEL so spraying wrong PINs on one surface (e.g. SSH) cannot throttle the
// legitimate admin on another (HTTP login) — a cross-channel login DoS.
//
// AdminAuth keeps an in-process credential + lockout state on host (no NVS), so the
// rate-limit logic is fully host-testable. Tests run sequentially in one binary and
// share that static state, so each test re-provisions the PIN (which clears every
// channel's lockout) to start from a known clean baseline.

#include <gtest/gtest.h>
#include <string>
#include "AdminAuth.hpp"

namespace {
constexpr const char* kPin   = "13579";   // >= kMinPinLength
constexpr const char* kWrong = "00000";

using AdminAuth::Channel;

// Re-provision the PIN -> resets every channel's failure counter + cooldown.
void resetAuth() { ASSERT_TRUE(AdminAuth::setPin(kPin)); }

// Trip a channel's lockout by exhausting kMaxFailedAttempts wrong PINs.
void tripLockout(Channel ch) {
    for (int i = 0; i < AdminAuth::kMaxFailedAttempts; ++i) {
        EXPECT_FALSE(AdminAuth::verifyPin(kWrong, ch));
    }
}
} // namespace

// A flood of wrong PINs over SSH must NOT lock the HTTP login channel.
TEST(AdminAuthLockout, SshBruteForceDoesNotLockHttp)
{
    resetAuth();
    tripLockout(Channel::Ssh);

    EXPECT_TRUE(AdminAuth::isLockedOut(Channel::Ssh))
        << "the SSH channel must be locked after kMaxFailedAttempts";
    EXPECT_FALSE(AdminAuth::isLockedOut(Channel::Http))
        << "the HTTP channel must remain UNLOCKED (no cross-channel DoS)";

    // The legitimate admin can still log in over HTTP with the correct PIN.
    EXPECT_TRUE(AdminAuth::verifyPin(kPin, Channel::Http))
        << "a correct PIN on HTTP must still succeed while SSH is locked";
}

// And the reverse: HTTP spraying must not lock SSH or DTMF.
TEST(AdminAuthLockout, HttpBruteForceDoesNotLockSshOrDtmf)
{
    resetAuth();
    tripLockout(Channel::Http);

    EXPECT_TRUE(AdminAuth::isLockedOut(Channel::Http));
    EXPECT_FALSE(AdminAuth::isLockedOut(Channel::Ssh));
    EXPECT_FALSE(AdminAuth::isLockedOut(Channel::Dtmf));

    EXPECT_TRUE(AdminAuth::verifyPin(kPin, Channel::Ssh));
    EXPECT_TRUE(AdminAuth::verifyPin(kPin, Channel::Dtmf));
}

// While a channel is locked, even the CORRECT PIN is refused on that channel
// (the lockout is honored without hashing) — but other channels are unaffected.
TEST(AdminAuthLockout, LockedChannelRefusesEvenCorrectPin)
{
    resetAuth();
    tripLockout(Channel::Dtmf);

    EXPECT_TRUE(AdminAuth::isLockedOut(Channel::Dtmf));
    EXPECT_FALSE(AdminAuth::verifyPin(kPin, Channel::Dtmf))
        << "a locked channel must refuse even the correct PIN during cooldown";
    EXPECT_TRUE(AdminAuth::verifyPin(kPin, Channel::Http))
        << "other channels stay usable";
}

// A successful login resets only its own channel's counter; re-setting the PIN
// clears all channels (covered implicitly by resetAuth at the top of each test).
TEST(AdminAuthLockout, CorrectPinResetsItsChannelCounter)
{
    resetAuth();
    // A few (but < kMaxFailedAttempts) wrong tries, then a correct one.
    for (int i = 0; i < AdminAuth::kMaxFailedAttempts - 1; ++i) {
        EXPECT_FALSE(AdminAuth::verifyPin(kWrong, Channel::Ssh));
    }
    EXPECT_FALSE(AdminAuth::isLockedOut(Channel::Ssh));
    EXPECT_TRUE(AdminAuth::verifyPin(kPin, Channel::Ssh));   // resets the counter

    // The counter is back to zero, so a fresh full streak is needed to lock again:
    // one more wrong try must NOT immediately lock.
    EXPECT_FALSE(AdminAuth::verifyPin(kWrong, Channel::Ssh));
    EXPECT_FALSE(AdminAuth::isLockedOut(Channel::Ssh));
}

// ── Issue #130: within the Http channel, lockout is per SOURCE IP ────────────
// The old channel-global counter let any LAN device lock the legitimate admin
// out by firing 5 wrong PINs (THREAT_MODEL D-3 admin self-DoS). All clock-free.

namespace {
// Opaque IPv4 keys (network byte order is irrelevant to the accounting logic).
constexpr uint32_t kAttackerIp = 0x0100000A;   // "10.0.0.1"
constexpr uint32_t kAdminIp    = 0x0200000A;   // "10.0.0.2"

void tripHttpLockoutFromIp(uint32_t ip) {
    for (int i = 0; i < AdminAuth::kMaxFailedAttempts; ++i) {
        EXPECT_FALSE(AdminAuth::verifyPin(kWrong, Channel::Http, ip));
    }
}
} // namespace

// An attacker's failed streak locks out ONLY the attacker's IP; the admin's IP
// still logs in with the correct PIN — the D-3 self-DoS path is closed.
TEST(AdminAuthLockout, HttpLockoutIsPerSourceIp)
{
    resetAuth();
    tripHttpLockoutFromIp(kAttackerIp);

    EXPECT_TRUE(AdminAuth::isLockedOut(Channel::Http, kAttackerIp))
        << "the attacker's own IP must be locked after kMaxFailedAttempts";
    EXPECT_FALSE(AdminAuth::isLockedOut(Channel::Http, kAdminIp))
        << "a different source IP must remain unlocked";
    EXPECT_TRUE(AdminAuth::verifyPin(kPin, Channel::Http, kAdminIp))
        << "the admin logs in from their own IP while the attacker is locked";

    // The locked IP refuses even the correct PIN during its cooldown.
    EXPECT_FALSE(AdminAuth::verifyPin(kPin, Channel::Http, kAttackerIp));
}

// Legacy callers (no source info, srcIp==0) and non-Http channels keep the
// per-channel bucket, fully independent of the per-IP table.
TEST(AdminAuthLockout, LegacyAndNonHttpCallersUnaffectedByPerIpTable)
{
    resetAuth();
    tripHttpLockoutFromIp(kAttackerIp);

    // srcIp==0 → legacy channel-global bucket, untouched by the attacker's IP.
    EXPECT_FALSE(AdminAuth::isLockedOut(Channel::Http))
        << "the legacy (no-IP) Http bucket must not be tripped by per-IP failures";
    EXPECT_TRUE(AdminAuth::verifyPin(kPin, Channel::Http));

    // Non-Http channels ignore srcIp entirely (per-channel semantics, #57).
    tripLockout(Channel::Ssh);
    EXPECT_TRUE(AdminAuth::isLockedOut(Channel::Ssh, kAttackerIp))
        << "srcIp must be ignored on non-Http channels (same channel bucket)";
}

// Bounded table: at capacity (kMaxTrackedIps) the OLDEST-touched entry is
// evicted, so spoofed sources can only recycle slots — memory never grows —
// and an evicted source starts from a fresh window.
TEST(AdminAuthLockout, PerIpTableEvictsOldestAtCapacity)
{
    resetAuth();

    // Entry 1: partially tripped (kMax-1 failures — one more would lock it).
    const uint32_t oldest = 0x01010101;
    for (int i = 0; i < AdminAuth::kMaxFailedAttempts - 1; ++i) {
        EXPECT_FALSE(AdminAuth::verifyPin(kWrong, Channel::Http, oldest));
    }

    // Fill the remaining kMaxTrackedIps-1 slots, then one more to force the
    // eviction of `oldest` (it has the earliest last-touch).
    for (uint32_t i = 0; i < AdminAuth::kMaxTrackedIps; ++i) {
        EXPECT_FALSE(AdminAuth::verifyPin(kWrong, Channel::Http, 0x0A000000u + i));
    }

    // `oldest` was evicted: its streak is gone, so a single wrong PIN must not
    // lock it (a fresh window), and it reports unlocked.
    EXPECT_FALSE(AdminAuth::isLockedOut(Channel::Http, oldest));
    EXPECT_FALSE(AdminAuth::verifyPin(kWrong, Channel::Http, oldest));
    EXPECT_FALSE(AdminAuth::isLockedOut(Channel::Http, oldest))
        << "an evicted IP must restart from a clean window, not a stale streak";

    // The newest entries are still tracked: the last-added IP can be locked by
    // completing its streak.
    const uint32_t newest = 0x0A000000u + AdminAuth::kMaxTrackedIps - 1;
    for (int i = 0; i < AdminAuth::kMaxFailedAttempts - 1; ++i) {
        EXPECT_FALSE(AdminAuth::verifyPin(kWrong, Channel::Http, newest));
    }
    EXPECT_TRUE(AdminAuth::isLockedOut(Channel::Http, newest));
}
