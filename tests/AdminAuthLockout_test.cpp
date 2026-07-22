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

// --- Issue #130: per-source-IP lockout scoping within the Http channel ---------

namespace {
constexpr uint32_t kAttackerIp = 0x0A000001;  // 10.0.0.1
constexpr uint32_t kAdminIp    = 0x0A000002;  // 10.0.0.2

void tripLockoutForIp(uint32_t ip) {
    for (int i = 0; i < AdminAuth::kMaxFailedAttempts; ++i) {
        EXPECT_FALSE(AdminAuth::verifyPin(kWrong, Channel::Http, ip));
    }
}
} // namespace

// An attacker IP spraying wrong PINs must not lock out a different IP's admin —
// the whole point of issue #130 (D-3 in THREAT_MODEL.md: a shared global HTTP
// lockout let an attacker self-DoS the legitimate admin).
TEST(AdminAuthLockout, AttackerIpDoesNotLockOutAdminIp)
{
    resetAuth();
    tripLockoutForIp(kAttackerIp);

    EXPECT_TRUE(AdminAuth::isLockedOut(Channel::Http, kAttackerIp))
        << "the attacker's own IP must be locked after kMaxFailedAttempts";
    EXPECT_FALSE(AdminAuth::isLockedOut(Channel::Http, kAdminIp))
        << "a different source IP must remain unlocked";

    EXPECT_TRUE(AdminAuth::verifyPin(kPin, Channel::Http, kAdminIp))
        << "the legitimate admin, from a different IP, can still log in";
}

// Passing sourceIp == 0 (the default) must fall back to the legacy channel-wide
// counter untouched by any per-IP activity — this is what every pre-#130 caller
// (and any test that doesn't care about IP scoping) still gets.
TEST(AdminAuthLockout, ZeroIpFallsBackToChannelWideLockout)
{
    resetAuth();
    tripLockoutForIp(kAttackerIp);

    // The per-IP table is separate from the legacy channel-wide counters, so a
    // caller that never passes an IP (sourceIp == 0) is unaffected by the above.
    EXPECT_FALSE(AdminAuth::isLockedOut(Channel::Http))
        << "channel-wide (IP-less) lockout state must be independent of per-IP state";
    EXPECT_TRUE(AdminAuth::verifyPin(kPin, Channel::Http));
}

// A correct PIN from a given IP resets ONLY that IP's counter, not others'.
TEST(AdminAuthLockout, CorrectPinResetsOnlyItsOwnIpCounter)
{
    resetAuth();
    for (int i = 0; i < AdminAuth::kMaxFailedAttempts - 1; ++i) {
        EXPECT_FALSE(AdminAuth::verifyPin(kWrong, Channel::Http, kAttackerIp));
    }
    EXPECT_TRUE(AdminAuth::verifyPin(kPin, Channel::Http, kAttackerIp));  // resets it
    EXPECT_FALSE(AdminAuth::isLockedOut(Channel::Http, kAttackerIp));

    // One more wrong try from that same IP must not immediately re-lock it.
    EXPECT_FALSE(AdminAuth::verifyPin(kWrong, Channel::Http, kAttackerIp));
    EXPECT_FALSE(AdminAuth::isLockedOut(Channel::Http, kAttackerIp));
}

// The per-IP table is bounded (kMaxIpLockoutEntries): spraying from more distinct
// IPs than the table can hold must not crash, hang, or grow without bound — the
// oldest entries are evicted to make room (same discipline as RequestsHandler's
// per-source-IP rate-limit buckets).
TEST(AdminAuthLockout, PerIpTableIsBoundedUnderManyDistinctIps)
{
    resetAuth();
    for (uint32_t i = 0; i < static_cast<uint32_t>(AdminAuth::kMaxIpLockoutEntries) * 4; ++i) {
        uint32_t ip = 0xC0A80000u + i;  // 192.168.0.0 + i, all distinct, never 0
        EXPECT_FALSE(AdminAuth::verifyPin(kWrong, Channel::Http, ip));
    }
    // The legitimate admin (an IP not in the flood) is still unaffected.
    EXPECT_TRUE(AdminAuth::verifyPin(kPin, Channel::Http, kAdminIp));
}
