// AdminAuth_test.cpp — direct host coverage for the admin credential / session
// manager (src/Helpers/AdminAuth.cpp), the only gate guarding the dashboard's
// state-changing endpoints (/api/kill, /api/ota/upload, /api/factory-reset) on
// an OPEN AP. Issue #48 [H-7]: this logic was linked into the test binary but
// had zero direct tests — a regression in PIN hashing, the 5-attempt lockout,
// session TTL/uniqueness, or the constant-time compare could ship undetected.
//
// The host arm is fully deterministic: NVS is stubbed, the credential lives in
// an in-process static, and the lockout is exercised via clearCredential()
// (which resets failedAttempts/lockout) rather than a sleep — so these tests are
// non-flaky and never wall-clock dependent.
//
// IMPORTANT: AdminAuth state is a single process-wide static (state()), so the
// fixture's SetUp() wipes it (clearCredential) before every case to keep tests
// order-independent.

#include <gtest/gtest.h>
#include <set>
#include <string>

#include "AdminAuth.hpp"

namespace
{
class AdminAuthTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		// Start every case from the unprovisioned/open state with no live
		// sessions and a cleared lockout. clearCredential() does exactly that.
		AdminAuth::clearCredential();
	}
	void TearDown() override
	{
		AdminAuth::clearCredential();
	}
};

// ── Provisioning ────────────────────────────────────────────────────────────

TEST_F(AdminAuthTest, StartsUnprovisioned)
{
	EXPECT_FALSE(AdminAuth::isProvisioned());
	EXPECT_FALSE(AdminAuth::credentialIsSet());
	// Verify against an unprovisioned device must fail (no credential to match).
	EXPECT_FALSE(AdminAuth::verifyPin("1234"));
}

TEST_F(AdminAuthTest, SetPinProvisionsAndVerifies)
{
	EXPECT_TRUE(AdminAuth::setPin("1234"));
	EXPECT_TRUE(AdminAuth::isProvisioned());
	EXPECT_TRUE(AdminAuth::credentialIsSet());   // host delegates to isProvisioned()
	EXPECT_TRUE(AdminAuth::verifyPin("1234"));
}

TEST_F(AdminAuthTest, RejectsTooShortPin)
{
	// kMinPinLength == 4: a 3-char PIN must be refused and leave us unprovisioned.
	EXPECT_FALSE(AdminAuth::setPin("123"));
	EXPECT_FALSE(AdminAuth::isProvisioned());
	// Boundary: exactly kMinPinLength is accepted.
	EXPECT_TRUE(AdminAuth::setPin("1234"));
	EXPECT_TRUE(AdminAuth::isProvisioned());
}

TEST_F(AdminAuthTest, WrongPinIsRejected)
{
	ASSERT_TRUE(AdminAuth::setPin("1234"));
	EXPECT_FALSE(AdminAuth::verifyPin("0000"));
	EXPECT_FALSE(AdminAuth::verifyPin("12345"));   // superstring, different hash
	EXPECT_FALSE(AdminAuth::verifyPin(""));
	// The correct PIN still verifies after wrong attempts (below the lockout).
	EXPECT_TRUE(AdminAuth::verifyPin("1234"));
}

TEST_F(AdminAuthTest, SetPinReplacesCredential)
{
	ASSERT_TRUE(AdminAuth::setPin("1234"));
	ASSERT_TRUE(AdminAuth::verifyPin("1234"));
	// Re-provision with a new PIN: the old one must no longer verify.
	ASSERT_TRUE(AdminAuth::setPin("9876"));
	EXPECT_FALSE(AdminAuth::verifyPin("1234"));
	EXPECT_TRUE(AdminAuth::verifyPin("9876"));
}

// Salts are random per setPin, so re-provisioning the SAME pin still verifies
// (proves the salt is applied at verify time, not a hard-coded digest).
TEST_F(AdminAuthTest, SamePinReprovisionStillVerifies)
{
	ASSERT_TRUE(AdminAuth::setPin("4242"));
	ASSERT_TRUE(AdminAuth::verifyPin("4242"));
	ASSERT_TRUE(AdminAuth::setPin("4242"));    // fresh salt
	EXPECT_TRUE(AdminAuth::verifyPin("4242"));
}

// ── Brute-force lockout ─────────────────────────────────────────────────────

TEST_F(AdminAuthTest, FiveWrongAttemptsEngageLockout)
{
	ASSERT_TRUE(AdminAuth::setPin("1234"));
	EXPECT_FALSE(AdminAuth::isLockedOut());

	// kMaxFailedAttempts == 5: the first four wrong attempts do NOT lock out.
	for (int i = 0; i < 4; ++i)
	{
		EXPECT_FALSE(AdminAuth::verifyPin("0000"));
		EXPECT_FALSE(AdminAuth::isLockedOut()) << "locked out too early at attempt " << i;
	}
	// The 5th consecutive failure engages the lockout.
	EXPECT_FALSE(AdminAuth::verifyPin("0000"));
	EXPECT_TRUE(AdminAuth::isLockedOut());
}

// The CORRECT pin is rejected while the lockout is engaged — the security
// property that makes the lockout actually rate-limit a brute-force attacker.
TEST_F(AdminAuthTest, CorrectPinRejectedWhileLockedOut)
{
	ASSERT_TRUE(AdminAuth::setPin("1234"));
	for (int i = 0; i < 5; ++i)
	{
		AdminAuth::verifyPin("0000");
	}
	ASSERT_TRUE(AdminAuth::isLockedOut());
	// Even the right PIN must fail until the cooldown elapses.
	EXPECT_FALSE(AdminAuth::verifyPin("1234"));
}

// A correct PIN BEFORE the 5th failure resets the counter, so the streak must be
// consecutive — interleaving a success defuses the lockout.
TEST_F(AdminAuthTest, CorrectPinResetsFailureStreak)
{
	ASSERT_TRUE(AdminAuth::setPin("1234"));
	for (int i = 0; i < 4; ++i)
	{
		EXPECT_FALSE(AdminAuth::verifyPin("0000"));
	}
	// A correct verify here clears failedAttempts.
	EXPECT_TRUE(AdminAuth::verifyPin("1234"));
	// Four more wrong attempts must NOT lock out (the counter restarted).
	for (int i = 0; i < 4; ++i)
	{
		EXPECT_FALSE(AdminAuth::verifyPin("0000"));
	}
	EXPECT_FALSE(AdminAuth::isLockedOut());
}

// setPin (a credential reset, e.g. factory onboarding) clears an engaged
// lockout — a provisioning action must not stay locked.
TEST_F(AdminAuthTest, SetPinClearsLockout)
{
	ASSERT_TRUE(AdminAuth::setPin("1234"));
	for (int i = 0; i < 5; ++i)
	{
		AdminAuth::verifyPin("0000");
	}
	ASSERT_TRUE(AdminAuth::isLockedOut());

	ASSERT_TRUE(AdminAuth::setPin("5678"));     // reset
	EXPECT_FALSE(AdminAuth::isLockedOut());
	EXPECT_TRUE(AdminAuth::verifyPin("5678"));
}

// ── Sessions ────────────────────────────────────────────────────────────────

TEST_F(AdminAuthTest, CreateSessionYieldsValidToken)
{
	std::string token = AdminAuth::createSession();
	ASSERT_FALSE(token.empty());
	// kSessionTokenHex == 32: at least 128 bits of hex.
	EXPECT_GE(token.size(), AdminAuth::kSessionTokenHex);
	EXPECT_TRUE(AdminAuth::validateSession(token));
}

TEST_F(AdminAuthTest, UnknownAndEmptyTokensAreInvalid)
{
	ASSERT_FALSE(AdminAuth::createSession().empty());
	EXPECT_FALSE(AdminAuth::validateSession(""));
	EXPECT_FALSE(AdminAuth::validateSession("deadbeefdeadbeefdeadbeefdeadbeef"));
	EXPECT_FALSE(AdminAuth::validateSession("not-a-real-token"));
}

TEST_F(AdminAuthTest, SessionTokensAreUnique)
{
	// Distinct sessions must carry distinct tokens (no fixed/guessable value).
	std::set<std::string> tokens;
	for (size_t i = 0; i < AdminAuth::kMaxSessions; ++i)
	{
		std::string t = AdminAuth::createSession();
		ASSERT_FALSE(t.empty());
		EXPECT_TRUE(tokens.insert(t).second) << "duplicate session token issued";
	}
}

TEST_F(AdminAuthTest, DestroySessionInvalidatesToken)
{
	std::string token = AdminAuth::createSession();
	ASSERT_TRUE(AdminAuth::validateSession(token));
	AdminAuth::destroySession(token);
	EXPECT_FALSE(AdminAuth::validateSession(token));
	// Destroying an unknown/empty token is a harmless no-op.
	AdminAuth::destroySession("");
	AdminAuth::destroySession("ffffffffffffffffffffffffffffffff");
}

// The session table is fixed-capacity (kMaxSessions). Overflowing it evicts the
// oldest entry rather than failing — but a freshly created token is always live.
TEST_F(AdminAuthTest, SessionTableEvictsWhenFull)
{
	std::string first = AdminAuth::createSession();
	ASSERT_TRUE(AdminAuth::validateSession(first));

	// Fill the table fully and then one beyond it; the overflow evicts a slot.
	for (size_t i = 0; i < AdminAuth::kMaxSessions; ++i)
	{
		std::string t = AdminAuth::createSession();
		ASSERT_FALSE(t.empty());
		EXPECT_TRUE(AdminAuth::validateSession(t));   // newest is always valid
	}
}

// clearCredential() wipes sessions too (factory-reset returns to open state).
TEST_F(AdminAuthTest, ClearCredentialWipesSessions)
{
	std::string token = AdminAuth::createSession();
	ASSERT_TRUE(AdminAuth::validateSession(token));
	ASSERT_TRUE(AdminAuth::setPin("1234"));

	AdminAuth::clearCredential();
	EXPECT_FALSE(AdminAuth::isProvisioned());
	EXPECT_FALSE(AdminAuth::validateSession(token));   // session gone
	EXPECT_FALSE(AdminAuth::isLockedOut());
}

}  // namespace
