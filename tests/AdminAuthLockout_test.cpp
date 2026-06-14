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
