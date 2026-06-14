// OtaUpdater_test.cpp — host coverage for the OTA session state machine
// (src/Helpers/OtaUpdater.cpp). Issue #62 [M-10]: test_api.sh asserts the host
// OTA *endpoint* returns 501, but the OtaUpdater state machine that backs both
// the host stub AND the device flash path (begin/write/end/activate/abort, the
// pending-update flag, bytesWritten accounting, and the error guards on
// out-of-order calls) had no direct test. The HOST arm of that state machine is
// deterministic and fully exercisable; the real on-device esp_ota_* flashing +
// rollback remains hardware-only (deferred to final on-COM5 verify).
//
// These assertions pin the call-ordering contract that handleOtaUpload() relies
// on, so a regression in the lifecycle guards is caught on the host build rather
// than at flash time.

#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <vector>

#include "OtaUpdater.hpp"

namespace
{
// ── happy-path lifecycle ────────────────────────────────────────────────────

TEST(OtaUpdater, FullLifecycleHostStub)
{
	OtaUpdater ota;
	EXPECT_FALSE(ota.isInProgress());
	EXPECT_FALSE(ota.isUpdatePending());
	EXPECT_EQ(ota.bytesWritten(), 0u);

	ASSERT_TRUE(ota.begin(1024));
	EXPECT_TRUE(ota.isInProgress());

	std::vector<uint8_t> chunk(256, 0xAB);
	ASSERT_TRUE(ota.write(chunk.data(), chunk.size()));
	ASSERT_TRUE(ota.write(chunk.data(), chunk.size()));
	EXPECT_EQ(ota.bytesWritten(), 512u);   // accumulates across writes

	ASSERT_TRUE(ota.end());
	EXPECT_FALSE(ota.isInProgress());
	EXPECT_FALSE(ota.isUpdatePending());   // not pending until activate()

	ASSERT_TRUE(ota.activate());
	EXPECT_TRUE(ota.isUpdatePending());    // a new image is staged for next boot
	EXPECT_TRUE(ota.lastError().empty());
}

// ── ordering guards (the contract handleOtaUpload depends on) ───────────────

TEST(OtaUpdater, WriteBeforeBeginFails)
{
	OtaUpdater ota;
	uint8_t b = 0;
	EXPECT_FALSE(ota.write(&b, 1));
	EXPECT_FALSE(ota.lastError().empty());   // surfaces a diagnostic
}

TEST(OtaUpdater, EndBeforeBeginFails)
{
	OtaUpdater ota;
	EXPECT_FALSE(ota.end());
	EXPECT_FALSE(ota.lastError().empty());
}

TEST(OtaUpdater, ActivateBeforeEndFails)
{
	OtaUpdater ota;
	ASSERT_TRUE(ota.begin(64));
	// A session still in progress must not be activatable.
	EXPECT_FALSE(ota.activate());
	EXPECT_FALSE(ota.lastError().empty());
	EXPECT_FALSE(ota.isUpdatePending());
}

TEST(OtaUpdater, DoubleBeginFails)
{
	OtaUpdater ota;
	ASSERT_TRUE(ota.begin(64));
	EXPECT_FALSE(ota.begin(64));   // already in progress
	EXPECT_FALSE(ota.lastError().empty());
}

// ── abort / re-begin ────────────────────────────────────────────────────────

TEST(OtaUpdater, AbortClearsSessionAndAllowsRebegin)
{
	OtaUpdater ota;
	ASSERT_TRUE(ota.begin(128));
	uint8_t b[4] = {1, 2, 3, 4};
	ASSERT_TRUE(ota.write(b, sizeof(b)));
	EXPECT_TRUE(ota.isInProgress());

	ota.abort();
	EXPECT_FALSE(ota.isInProgress());

	// A fresh session after abort starts clean (byte counter resets on begin).
	ASSERT_TRUE(ota.begin(256));
	EXPECT_EQ(ota.bytesWritten(), 0u);
	EXPECT_TRUE(ota.isInProgress());
}

TEST(OtaUpdater, AbortWhenIdleIsSafe)
{
	OtaUpdater ota;
	ota.abort();                       // no session open — must be a harmless no-op
	EXPECT_FALSE(ota.isInProgress());
}

// Empty/zero-length writes are no-op successes (a 0-byte recv slice).
TEST(OtaUpdater, EmptyWriteIsNoopSuccess)
{
	OtaUpdater ota;
	ASSERT_TRUE(ota.begin(16));
	EXPECT_TRUE(ota.write(nullptr, 0));
	EXPECT_EQ(ota.bytesWritten(), 0u);
}

// begin(0) (unknown size) is accepted — Content-Length is optional.
TEST(OtaUpdater, BeginWithUnknownSize)
{
	OtaUpdater ota;
	ASSERT_TRUE(ota.begin(0));
	EXPECT_TRUE(ota.isInProgress());
}

// ── static status helpers (host placeholders, but must be stable/total) ─────

TEST(OtaUpdater, StaticStatusHelpersHost)
{
	// On host these are fixed placeholders; assert they are non-empty and that
	// the verify/markValid contract holds (nothing pending, markValid succeeds).
	EXPECT_FALSE(OtaUpdater::runningPartitionLabel().empty());
	EXPECT_FALSE(OtaUpdater::nextUpdatePartitionLabel().empty());
	EXPECT_FALSE(OtaUpdater::bootPartitionLabel().empty());
	EXPECT_FALSE(OtaUpdater::isPendingVerify());   // host never pending
	EXPECT_TRUE(OtaUpdater::markValid());          // idempotent no-op success
}

}  // namespace
