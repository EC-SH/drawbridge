// PoolSafety_test.cpp — audit concurrency & memory-safety cluster
// (issues #41 C-1, #54 M-2, #70 L-6, #69 L-5).
//
// These tests assert the pool-discipline invariants the audit fixes rely on:
//   * #41: getMessageFromPool() is safe to call concurrently from multiple threads
//          (it now holds a dedicated leaf mutex around the check-then-act slot scan).
//          A racing draw must never hand the SAME pooled message to two callers at
//          once, and must never corrupt a buffer mid-reset.
//   * #54: the message pool is sized to cover the worst-case broadcast + BLF-NOTIFY
//          burst (MAX_CLIENTS + MAX_SUBSCRIPTIONS), so peak fan-out stays off the
//          hot-path heap fallback.
//   * #70: virtual-peer SipClients (777/440/park/PSTN legs) come from a fixed pool;
//          driving repeated 777 echo setups keeps the handler functional and does not
//          crash on (or even reach) the heap fallback within pool capacity.

#include <gtest/gtest.h>
#include "RequestsHandler.hpp"
#include "SipMessage.hpp"
#include "SipMessageTypes.h"
#include "PoolConfig.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

sockaddr_in mkAddr(const std::string& ip, uint16_t port = 5060) {
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = inet_addr(ip.c_str());
    s.sin_port = htons(port);
    return s;
}

} // namespace

// ── #41: concurrent draw from the static message pool ────────────────────────────
// Before the fix, getMessageFromPool() did a lock-free check-then-act on the static
// _messagePool from the UDP RX thread AND from tick()/handlers, so two threads could
// observe the same idle slot and both reset+return it. Hammer the pool from several
// threads; while a thread holds its drawn message, no OTHER thread may hold the same
// pooled object. We detect a double-hand-out via a per-slot atomic "owned" flag keyed
// on the raw pointer.
TEST(PoolSafety, ConcurrentMessagePoolDrawNeverDoubleHandsOut) {
    constexpr int kThreads = 8;
    constexpr int kIters   = 4000;

    std::atomic<bool> doubleHandout{false};
    std::atomic<bool> corrupted{false};
    // Guard map of pooled-pointer -> in-use flag. We only ever INSERT pointers the
    // pool hands us; the pool is fixed-size so the set stays small.
    std::mutex seenMutex;
    std::map<const SipMessage*, std::atomic<bool>> inUse;

    auto worker = [&](int id) {
        for (int i = 0; i < kIters; ++i) {
            std::string payload =
                "OPTIONS sip:server SIP/2.0\r\n"
                "Via: SIP/2.0/UDP 10.0.0." + std::to_string(id) + ":5060;branch=z9hG4bK" +
                std::to_string(i) + "\r\n"
                "From: <sip:t" + std::to_string(id) + "@server>;tag=x\r\n"
                "To: <sip:server>\r\n"
                "Call-ID: c" + std::to_string(id) + "-" + std::to_string(i) + "\r\n"
                "CSeq: 1 OPTIONS\r\n"
                "Content-Length: 0\r\n\r\n";

            auto msg = RequestsHandler::getMessageFromPool(payload, mkAddr("10.0.0.1"));
            const SipMessage* key = msg.get();

            std::atomic<bool>* flag = nullptr;
            {
                std::lock_guard<std::mutex> lk(seenMutex);
                flag = &inUse[key];   // default-constructs to false on first sight
            }
            // Claim the slot. If it was already claimed, two threads hold the same
            // pooled object simultaneously — the exact #41 data race.
            bool expected = false;
            if (!flag->compare_exchange_strong(expected, true)) {
                doubleHandout.store(true);
            }

            // The message must reflect OUR payload — a torn reset would leave a Call-ID
            // from another draw. We check our unique Call-ID round-trips.
            std::string wantCallId = "c" + std::to_string(id) + "-" + std::to_string(i);
            if (std::string(msg->getCallID()).find(wantCallId) == std::string::npos) {
                corrupted.store(true);
            }

            // Hold briefly so overlap is likely, then release the slot before dropping
            // our shared_ptr ref (so the pool can recycle it).
            std::this_thread::yield();
            flag->store(false);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    EXPECT_FALSE(doubleHandout.load()) << "two threads held the same pooled SipMessage at once (#41)";
    EXPECT_FALSE(corrupted.load())     << "a pooled message was reset mid-use (#41 torn buffer)";
}

// ── #54: the message pool covers the worst-case fan-out + NOTIFY burst ────────────
// A 999 all-page forks one pooled INVITE per client while refreshSubscriptions()
// draws one NOTIFY per active subscription in the same locked section; both live in
// _outbox until it flushes. Sizing the pool >= MAX_CLIENTS + MAX_SUBSCRIPTIONS keeps
// that peak allocation-free. Assert the sizing invariant holds at compile time.
TEST(PoolSafety, MessagePoolCoversBroadcastPlusNotifyBurst) {
    static_assert(POCKETDIAL_MSG_POOL >= POCKETDIAL_MAX_CLIENTS + POCKETDIAL_MAX_SUBSCRIPTIONS,
                  "#54: message pool must cover the worst-case 999-page + BLF-NOTIFY burst");
    EXPECT_GE(POCKETDIAL_MSG_POOL, POCKETDIAL_MAX_CLIENTS + POCKETDIAL_MAX_SUBSCRIPTIONS);
}

// ── #70: virtual-peer pool keeps the handler alive across repeated echo setups ────
// Each 777 echo call setup needs a transient virtual peer; historically make_shared'd
// in the packet handler, now drawn from _virtualPeerPool. Drive several sequential
// 777 calls (tearing each down so the session — and thus its virtual peer — recycles)
// and assert every one is answered, proving the pool recycles correctly and never
// faults.
TEST(PoolSafety, VirtualPeerPoolServesRepeated777Echo) {
    std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> out;
    RequestsHandler handler("192.168.9.1", 5060,
        [&](const sockaddr_in& dest, std::shared_ptr<SipMessage> msg) {
            out.emplace_back(dest, msg);
        });

    const std::string ip = "192.168.9.50";
    // Register a real extension to originate the echo calls.
    {
        std::string reg =
            "REGISTER sip:server SIP/2.0\r\n"
            "Via: SIP/2.0/UDP " + ip + ":5060;branch=z9hG4bKr\r\n"
            "From: <sip:200@server>;tag=rt200\r\n"
            "To: <sip:200@server>\r\n"
            "Call-ID: reg-200\r\n"
            "CSeq: 1 REGISTER\r\n"
            "Contact: <sip:200@" + ip + ":5060>;expires=3600\r\n"
            "Content-Length: 0\r\n\r\n";
        handler.handle(RequestsHandler::getMessageFromPool(reg, mkAddr(ip)));
    }

    // Vary the SOURCE IP per call so the per-source token-bucket rate limiter (Issue
    // #38: capacity 40, 20/s) never throttles — this test is about pool recycling, not
    // rate limiting. The caller is identified by its From EXTENSION ("200"), which is
    // registered, so a fresh source IP still routes the 777 echo.
    auto srcFor = [&](int i) { return mkAddr("10.10." + std::to_string(i / 250) + "." + std::to_string(1 + i % 250)); };

    auto inviteTo777 = [&](const std::string& callId, int i) {
        std::string body =
            "v=0\r\no=- 0 0 IN IP4 " + ip + "\r\ns=-\r\nc=IN IP4 " + ip +
            "\r\nt=0 0\r\nm=audio 40000 RTP/AVP 0 8 101\r\n";
        std::string head =
            "INVITE sip:777@server SIP/2.0\r\n"
            "Via: SIP/2.0/UDP " + ip + ":5060;branch=z9hG4bK" + callId + "\r\n"
            "From: <sip:200@server>;tag=ft200\r\n"
            "To: <sip:777@server>\r\n"
            "Call-ID: " + callId + "\r\n"
            "CSeq: 1 INVITE\r\n"
            "Contact: <sip:200@" + ip + ":5060>\r\n"
            "Content-Type: application/sdp\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        return RequestsHandler::getMessageFromPool(head + body, srcFor(i));
    };
    auto byeFor = [&](const std::string& callId, int i) {
        std::string bye =
            "BYE sip:777@server SIP/2.0\r\n"
            "Via: SIP/2.0/UDP " + ip + ":5060;branch=z9hG4bKb" + callId + "\r\n"
            "From: <sip:200@server>;tag=ft200\r\n"
            "To: <sip:777@server>;tag=echotag\r\n"
            "Call-ID: " + callId + "\r\n"
            "CSeq: 2 BYE\r\n"
            "Content-Length: 0\r\n\r\n";
        return RequestsHandler::getMessageFromPool(bye, srcFor(i));
    };

    // Many more iterations than the pool depth: each call is set up then torn down, so
    // a correctly-recycling pool serves them all from a handful of slots.
    const int kCalls = POCKETDIAL_VIRTUAL_PEERS * 3 + 5;
    int answered = 0;
    for (int i = 0; i < kCalls; ++i) {
        const std::string callId = "echo-" + std::to_string(i);
        out.clear();
        handler.handle(inviteTo777(callId, i));
        // 777 auto-answers: a 200 OK back to the caller proves the virtual peer was
        // allocated and the session connected.
        bool ok = false;
        for (auto& p : out) {
            if (p.second->toString().rfind("SIP/2.0 200", 0) == 0) { ok = true; break; }
        }
        if (ok) ++answered;
        // Tear down so the session (and its pooled virtual peer) is released for reuse.
        handler.handle(byeFor(callId, i));
    }

    EXPECT_EQ(answered, kCalls) << "every recycled 777 echo setup must be answered (#70 pool reuse)";
}
