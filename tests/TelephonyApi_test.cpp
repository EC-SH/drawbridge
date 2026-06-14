// TelephonyApi_test.cpp — host coverage for the anchor credential layer:
// TelephonyApiConfig (the bounded per-provider credential table) and
// TelephonyProvider (the type→implementation registry that selects the boot-time
// WAN anchor). Issue #61 [M-9]: both .cpp are linked into the test binary (for
// RequestsHandler) but had no test file; kSlots is a hard pool bound and the
// active-slot selection drives which provider dials at boot.
//
// Host backend: TelephonyApiConfig persists to a 0600 key=value file; tests point
// it at a unique temp path via setStorePath so they never touch the developer's
// working-dir config and stay isolated from one another.

#include <gtest/gtest.h>
#include <cstdio>
#include <string>

#include "TelephonyApiConfig.hpp"
#include "TelephonyProvider.hpp"
#include "AnchorClient.hpp"

namespace
{
// A unique temp store path per test instance so cases don't share a file.
std::string tempStorePath(const char* tag)
{
	std::string p = std::string("pd_tapi_test_") + tag + ".cfg";
	std::remove(p.c_str());   // start clean
	return p;
}

TelephonyApiConfig::Slot makeSlot(TelephonyProviderType t, bool enabled,
                                  const std::string& url, const std::string& id,
                                  const std::string& secret, const std::string& dn)
{
	TelephonyApiConfig::Slot s;
	s.type = t; s.enabled = enabled;
	s.baseUrl = url; s.clientId = id; s.secret = secret; s.routeDn = dn;
	return s;
}

// ── set/get round-trip + display-safe projection ────────────────────────────

TEST(TelephonyApiConfig, SetGetRoundTrip)
{
	TelephonyApiConfig cfg;
	cfg.setStorePath(tempStorePath("roundtrip"));

	auto s = makeSlot(TelephonyProviderType::ThreeCx, true,
	                  "https://pbx.example.com", "client-id", "s3cr3t", "100");
	ASSERT_EQ(cfg.setSlot(0, s, /*keepSecret=*/false), "");   // "" == success

	TelephonyApiConfig::SlotView v = cfg.view(0);
	EXPECT_EQ(v.type, TelephonyProviderType::ThreeCx);
	EXPECT_TRUE(v.enabled);
	EXPECT_TRUE(v.implemented);          // 3CX is a real provider
	EXPECT_EQ(v.baseUrl, "https://pbx.example.com");
	EXPECT_EQ(v.clientId, "client-id");
	EXPECT_EQ(v.routeDn, "100");
	EXPECT_TRUE(v.secretSet);            // set-or-not only

	// The engine-only accessor still returns the plaintext secret.
	const TelephonyApiConfig::Slot* boot = cfg.bootSlot(0);
	ASSERT_NE(boot, nullptr);
	EXPECT_EQ(boot->secret, "s3cr3t");
}

// SlotView is the display-safe projection: it carries secretSet but NEVER the
// secret value itself. This is the write-only-secret invariant.
TEST(TelephonyApiConfig, SlotViewNeverExposesSecret)
{
	TelephonyApiConfig cfg;
	cfg.setStorePath(tempStorePath("secret"));
	auto s = makeSlot(TelephonyProviderType::ThreeCx, false,
	                  "https://x", "id", "TOP-SECRET", "100");
	ASSERT_EQ(cfg.setSlot(1, s, false), "");

	TelephonyApiConfig::SlotView v = cfg.view(1);
	EXPECT_TRUE(v.secretSet);
	// SlotView has no secret member at all; assert the surrounding display fields
	// don't leak it and the engine accessor is the only place it lives.
	EXPECT_EQ(v.clientId.find("TOP-SECRET"), std::string::npos);
	EXPECT_EQ(v.baseUrl.find("TOP-SECRET"), std::string::npos);
	EXPECT_EQ(cfg.bootSlot(1)->secret, "TOP-SECRET");
}

// keepSecret=true means "the UI sent an empty field for unchanged" — the stored
// secret must survive an edit that updates other fields.
TEST(TelephonyApiConfig, KeepSecretPreservesStoredValue)
{
	TelephonyApiConfig cfg;
	cfg.setStorePath(tempStorePath("keepsecret"));
	ASSERT_EQ(cfg.setSlot(0, makeSlot(TelephonyProviderType::ThreeCx, false,
	                                  "https://x", "id", "orig-secret", "100"), false), "");

	// Edit the routeDn but pass an empty secret with keepSecret=true.
	auto edit = makeSlot(TelephonyProviderType::ThreeCx, false, "https://x", "id", "", "200");
	ASSERT_EQ(cfg.setSlot(0, edit, /*keepSecret=*/true), "");
	EXPECT_EQ(cfg.view(0).routeDn, "200");
	EXPECT_EQ(cfg.bootSlot(0)->secret, "orig-secret");   // preserved
	EXPECT_TRUE(cfg.view(0).secretSet);
}

// ── kSlots hard bound ───────────────────────────────────────────────────────

TEST(TelephonyApiConfig, RejectsOutOfRangeSlotIndex)
{
	TelephonyApiConfig cfg;
	cfg.setStorePath(tempStorePath("oob"));
	auto s = makeSlot(TelephonyProviderType::Loopback, false, "", "", "", "");

	// kSlots is the hard table bound; idx >= kSlots must be rejected, not crash.
	EXPECT_NE(cfg.setSlot(TelephonyApiConfig::kSlots, s, false), "");
	EXPECT_NE(cfg.clearSlot(TelephonyApiConfig::kSlots), "");
	EXPECT_EQ(cfg.bootSlot(TelephonyApiConfig::kSlots), nullptr);
	// setActiveSlot treats kNoActiveSlot (== kSlots) as the valid "clear" sentinel,
	// so a genuine out-of-range index is kSlots+1.
	EXPECT_NE(cfg.setActiveSlot(TelephonyApiConfig::kSlots + 1), "");
	EXPECT_EQ(cfg.setActiveSlot(TelephonyApiConfig::kNoActiveSlot), "");   // sentinel ok

	// view() of an out-of-range slot returns a default (safe) SlotView.
	TelephonyApiConfig::SlotView v = cfg.view(TelephonyApiConfig::kSlots);
	EXPECT_FALSE(v.enabled);
	EXPECT_FALSE(v.secretSet);
}

// Every valid slot 0..kSlots-1 is independently writable.
TEST(TelephonyApiConfig, AllSlotsUsable)
{
	TelephonyApiConfig cfg;
	cfg.setStorePath(tempStorePath("allslots"));
	for (size_t i = 0; i < TelephonyApiConfig::kSlots; ++i)
	{
		auto s = makeSlot(TelephonyProviderType::Loopback, false,
		                  "", "id" + std::to_string(i), "", "");
		EXPECT_EQ(cfg.setSlot(i, s, false), "") << "slot " << i << " rejected";
		EXPECT_EQ(cfg.view(i).clientId, "id" + std::to_string(i));
	}
}

// ── validation rules ────────────────────────────────────────────────────────

TEST(TelephonyApiConfig, EnabledRealProviderRequiresHttps)
{
	TelephonyApiConfig cfg;
	cfg.setStorePath(tempStorePath("https"));
	// An ENABLED non-loopback slot must have an https:// base URL.
	auto bad = makeSlot(TelephonyProviderType::ThreeCx, true, "http://insecure", "id", "x", "100");
	EXPECT_NE(cfg.setSlot(0, bad, false), "");
	// Loopback is exempt (on-box, no PSTN).
	auto loop = makeSlot(TelephonyProviderType::Loopback, true, "", "", "", "");
	EXPECT_EQ(cfg.setSlot(0, loop, false), "");
}

TEST(TelephonyApiConfig, RejectsOverlongFields)
{
	TelephonyApiConfig cfg;
	cfg.setStorePath(tempStorePath("toolong"));
	auto s = makeSlot(TelephonyProviderType::Loopback, false,
	                  std::string(TelephonyApiConfig::kMaxUrlLen + 1, 'x'), "", "", "");
	EXPECT_NE(cfg.setSlot(0, s, false), "");   // URL too long
}

// ── active-slot selection (the boot anchor source) ──────────────────────────

TEST(TelephonyApiConfig, ActiveSlotSelectionAndClear)
{
	TelephonyApiConfig cfg;
	cfg.setStorePath(tempStorePath("active"));
	EXPECT_EQ(cfg.activeSlot(), TelephonyApiConfig::kNoActiveSlot);   // none by default

	ASSERT_EQ(cfg.setActiveSlot(2), "");
	EXPECT_EQ(cfg.activeSlot(), 2u);
	EXPECT_TRUE(cfg.view(2).active);
	EXPECT_FALSE(cfg.view(0).active);

	// kNoActiveSlot clears the selection (back to legacy/loopback behavior).
	ASSERT_EQ(cfg.setActiveSlot(TelephonyApiConfig::kNoActiveSlot), "");
	EXPECT_EQ(cfg.activeSlot(), TelephonyApiConfig::kNoActiveSlot);
}

// Clearing the active slot also drops the active marker.
TEST(TelephonyApiConfig, ClearActiveSlotDropsSelection)
{
	TelephonyApiConfig cfg;
	cfg.setStorePath(tempStorePath("clearactive"));
	ASSERT_EQ(cfg.setSlot(1, makeSlot(TelephonyProviderType::Loopback, false, "", "id", "", ""), false), "");
	ASSERT_EQ(cfg.setActiveSlot(1), "");
	ASSERT_EQ(cfg.activeSlot(), 1u);
	ASSERT_EQ(cfg.clearSlot(1), "");
	EXPECT_EQ(cfg.activeSlot(), TelephonyApiConfig::kNoActiveSlot);
}

// ── provider type metadata ──────────────────────────────────────────────────

TEST(TelephonyProvider, NameAndImplementedFlags)
{
	EXPECT_STREQ(telephonyProviderName(TelephonyProviderType::Loopback), "LOOPBACK");
	EXPECT_STREQ(telephonyProviderName(TelephonyProviderType::ThreeCx), "3CX");
	EXPECT_STREQ(telephonyProviderName(TelephonyProviderType::Count), "?");   // out of range

	// Only Loopback + 3CX are real; the rest are honest stubs.
	EXPECT_TRUE(telephonyProviderImplemented(TelephonyProviderType::Loopback));
	EXPECT_TRUE(telephonyProviderImplemented(TelephonyProviderType::ThreeCx));
	EXPECT_FALSE(telephonyProviderImplemented(TelephonyProviderType::Apidaze));
	EXPECT_FALSE(telephonyProviderImplemented(TelephonyProviderType::Sangoma));
}

// ── registry / provider selection ───────────────────────────────────────────

TEST(TelephonyProviderRegistry, RegisterAndSelect)
{
	TelephonyProviderRegistry reg;
	StubTelephonyProvider loopback(TelephonyProviderType::Loopback);
	StubTelephonyProvider threecx(TelephonyProviderType::ThreeCx);

	EXPECT_EQ(reg.select(TelephonyProviderType::Loopback), nullptr);   // none yet
	ASSERT_TRUE(reg.registerProvider(TelephonyProviderType::Loopback, &loopback));
	ASSERT_TRUE(reg.registerProvider(TelephonyProviderType::ThreeCx, &threecx));

	EXPECT_EQ(reg.select(TelephonyProviderType::Loopback), &loopback);
	EXPECT_EQ(reg.select(TelephonyProviderType::ThreeCx), &threecx);
	// Unregistered type -> nullptr (caller falls back to loopback).
	EXPECT_EQ(reg.select(TelephonyProviderType::Apidaze), nullptr);
}

TEST(TelephonyProviderRegistry, RejectsNullAndDoubleRegistration)
{
	TelephonyProviderRegistry reg;
	StubTelephonyProvider a(TelephonyProviderType::ThreeCx);
	StubTelephonyProvider b(TelephonyProviderType::ThreeCx);

	EXPECT_FALSE(reg.registerProvider(TelephonyProviderType::ThreeCx, nullptr));   // null rejected
	ASSERT_TRUE(reg.registerProvider(TelephonyProviderType::ThreeCx, &a));
	// Idempotent re-register of the SAME pointer is ok; a DIFFERENT one is refused.
	EXPECT_TRUE(reg.registerProvider(TelephonyProviderType::ThreeCx, &a));
	EXPECT_FALSE(reg.registerProvider(TelephonyProviderType::ThreeCx, &b));
	EXPECT_EQ(reg.select(TelephonyProviderType::ThreeCx), &a);   // unchanged
}

TEST(TelephonyProviderRegistry, OutOfRangeTypeIsSafe)
{
	TelephonyProviderRegistry reg;
	StubTelephonyProvider p(TelephonyProviderType::Loopback);
	EXPECT_FALSE(reg.registerProvider(TelephonyProviderType::Count, &p));   // sentinel
	EXPECT_EQ(reg.select(TelephonyProviderType::Count), nullptr);
}

}  // namespace
