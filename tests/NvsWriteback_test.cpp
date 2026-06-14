// NvsWriteback_test.cpp — regression coverage for the debounced NVS write-back
// (audit #42 H-1 / #55 M-3). The bug class: the persistXxx() config/CDR mutators
// performed a blocking nvs_open/commit (a 10-100+ ms flash erase/program) WHILE the
// caller held the registrar _mutex, stalling all SIP signaling. The fix moves the
// flash write off the lock: the mutators now only set a dirty bit, and tick() calls
// flushDirtyNvs() AFTER releasing _mutex.
//
// On host, NVS is compiled out (the in-memory maps ARE the store), so these tests
// can't observe the flash write itself. What they DO guard, on every host/CI run, is
// that re-routing every config mutation through the mark-dirty + tick()-flush path:
//   * keeps the in-memory config authoritative (no state dropped by the new flow),
//   * leaves tick() safe to call repeatedly with and without pending dirty state,
//   * round-trips set -> read -> clear -> read across ticks.
// i.e. the lock-discipline refactor is behaviour-preserving for the engine state.

#include <gtest/gtest.h>
#include "RequestsHandler.hpp"
#include "SipMessage.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

// Construct a RequestsHandler with a no-op send callback (these tests never inspect
// the wire — only the in-memory config state across the debounced-flush tick path).
RequestsHandler makeHandler() {
    return RequestsHandler("192.168.4.1", 5060,
        [](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
}

// Find an extension's forward tuple {ext, always, busy, noAnswer} in a getForwards()
// result, or return a sentinel with an empty ext if absent.
std::tuple<std::string, std::string, std::string, std::string>
findForward(RequestsHandler& h, const std::string& ext) {
    for (const auto& f : h.getForwards()) {
        if (std::get<0>(f) == ext) return f;
    }
    return {"", "", "", ""};
}

}  // namespace

// A forward set via the public mutator (which now marks dirty instead of writing NVS
// inline) is immediately readable, and a following tick() — which runs flushDirtyNvs()
// off the registrar lock — does NOT drop it.
TEST(NvsWriteback, ForwardSurvivesDebouncedFlushTick) {
    RequestsHandler handler = makeHandler();

    handler.setForward("100", "always", "200");
    auto before = findForward(handler, "100");
    ASSERT_EQ(std::get<0>(before), "100");
    EXPECT_EQ(std::get<1>(before), "200");  // always-target

    // tick() releases _mutex then calls flushDirtyNvs(); the in-memory map must persist.
    handler.tick();

    auto after = findForward(handler, "100");
    ASSERT_EQ(std::get<0>(after), "100");
    EXPECT_EQ(std::get<1>(after), "200");
}

// Clearing a trigger (empty target) is also a mark-dirty mutation; it must round-trip
// through a flush tick the same way a set does.
TEST(NvsWriteback, ForwardClearRoundTripsThroughTick) {
    RequestsHandler handler = makeHandler();

    handler.setForward("101", "busy", "300");
    handler.tick();
    EXPECT_EQ(std::get<2>(findForward(handler, "101")), "300");  // busy-target set

    handler.setForward("101", "busy", "");  // clear
    handler.tick();
    EXPECT_EQ(std::get<2>(findForward(handler, "101")), "");     // busy-target cleared
}

// A burst of mutations between two ticks coalesces into a single flush pass without
// losing any of the individual edits (the dirty bitmask is OR-accumulated, then the
// flush serializes the CURRENT map state once). Drives the ring-group + forward
// stores together so multiple dirty bits are pending at the same flush.
TEST(NvsWriteback, BurstOfMutationsAllVisibleAfterOneFlushTick) {
    RequestsHandler handler = makeHandler();

    handler.setForward("110", "always", "900");
    handler.setForward("111", "noanswer", "901");
    handler.setRingGroup("600", "110,111", "ringall");

    // One flush tick after the whole burst.
    handler.tick();

    EXPECT_EQ(std::get<1>(findForward(handler, "110")), "900");
    EXPECT_EQ(std::get<3>(findForward(handler, "111")), "901");  // noAnswer-target

    bool found600 = false;
    for (const auto& g : handler.getRingGroups()) {
        if (std::get<0>(g) == "600") {
            found600 = true;
            EXPECT_EQ(std::get<1>(g), "ringall");  // mode
        }
    }
    EXPECT_TRUE(found600);
}

// flushDirtyNvs() is invoked from tick() even when nothing is dirty; repeated ticks
// with no pending mutation must be a clean no-op (the early-out path), never a crash.
TEST(NvsWriteback, IdleTicksAreCleanNoOps) {
    RequestsHandler handler = makeHandler();
    for (int i = 0; i < 5; ++i) {
        handler.tick();
    }
    SUCCEED();
}
