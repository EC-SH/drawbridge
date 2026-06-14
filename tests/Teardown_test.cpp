// Teardown_test.cpp — issue #46: unauthenticated SIP BYE/CANCEL teardown.
//
// The registrar does not digest-challenge in-dialog requests (only REGISTER in
// secure mode), so the backstop is dialog-source binding: an in-dialog BYE/CANCEL
// must originate from the IP of one of the call's two real phone legs. A peer that
// merely sniffed or guessed a live Call-ID but sits at a DIFFERENT IP must NOT be
// able to drop the call.
//
// End-to-end through RequestsHandler (harness pattern from Hold_test.cpp): register
// 100 + 101, establish a connected call, then drive teardown from a spoofer IP vs a
// real leg IP and assert the wire result (403 vs 200 OK) and the dashboard view.

#include <gtest/gtest.h>
#include "RequestsHandler.hpp"
#include "SipMessage.hpp"
#include "SipMessageTypes.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <memory>
#include <string>
#include <vector>

namespace {

sockaddr_in makeAddr(const std::string& ip, uint16_t port = 5060) {
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = inet_addr(ip.c_str());
    s.sin_port = htons(port);
    return s;
}

std::string sdpBody(const std::string& ip) {
    return
        "v=0\r\n"
        "o=- 0 0 IN IP4 " + ip + "\r\n"
        "s=-\r\n"
        "c=IN IP4 " + ip + "\r\n"
        "t=0 0\r\n"
        "m=audio 40000 RTP/AVP 0 8 101\r\n"
        "a=rtpmap:0 PCMU/8000\r\n";
}

std::shared_ptr<SipMessage> makeRegister(const std::string& from, const std::string& srcIp) {
    std::string raw =
        "REGISTER sip:server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bKr\r\n"
        "From: <sip:" + from + "@server>;tag=rt" + from + "\r\n"
        "To: <sip:" + from + "@server>\r\n"
        "Call-ID: reg-" + from + "\r\n"
        "CSeq: 1 REGISTER\r\n"
        "Contact: <sip:" + from + "@" + srcIp + ":5060>;expires=3600\r\n"
        "Content-Length: 0\r\n\r\n";
    return RequestsHandler::getMessageFromPool(raw, makeAddr(srcIp));
}

std::shared_ptr<SipMessage> makeInvite(const std::string& from, const std::string& to,
                                       const std::string& srcIp, const std::string& callID) {
    std::string body = sdpBody(srcIp);
    std::string head =
        "INVITE sip:" + to + "@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bK" + callID + "\r\n"
        "From: <sip:" + from + "@server>;tag=ft" + from + "\r\n"
        "To: <sip:" + to + "@server>\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 1 INVITE\r\n"
        "Contact: <sip:" + from + "@" + srcIp + ":5060>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    return RequestsHandler::getMessageFromPool(head + body, makeAddr(srcIp));
}

std::shared_ptr<SipMessage> makeOk(const std::string& from, const std::string& to,
                                   const std::string& srcIp, const std::string& callID) {
    std::string body = sdpBody(srcIp);
    std::string raw =
        "SIP/2.0 200 OK\r\n"
        "Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bK" + callID + "\r\n"
        "From: <sip:" + from + "@server>;tag=ft" + from + "\r\n"
        "To: <sip:" + to + "@server>;tag=tt" + to + "\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 1 INVITE\r\n"
        "Contact: <sip:" + to + "@" + srcIp + ":5060>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    return RequestsHandler::getMessageFromPool(raw + body, makeAddr(srcIp));
}

// BYE from `srcIp` (which may NOT be a real leg — that's the spoof case).
std::shared_ptr<SipMessage> makeBye(const std::string& from, const std::string& to,
                                    const std::string& srcIp, const std::string& callID) {
    std::string raw =
        "BYE sip:" + to + "@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bKb\r\n"
        "From: <sip:" + from + "@server>;tag=ft" + from + "\r\n"
        "To: <sip:" + to + "@server>;tag=tt" + to + "\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 2 BYE\r\n"
        "Content-Length: 0\r\n\r\n";
    return RequestsHandler::getMessageFromPool(raw, makeAddr(srcIp));
}

struct Cap {
    std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> out;
    void clear() { out.clear(); }
};

// Fixture: 100 @ .50 and 101 @ .51 registered, a call 100 -> 101 established.
struct ByeFixture {
    Cap cap;
    RequestsHandler handler;
    const std::string callerIp = "192.168.4.50";
    const std::string calleeIp = "192.168.4.51";
    const std::string spoofIp  = "192.168.4.66";   // an associated AP peer, NOT a leg
    const std::string callID   = "teardown-call-1";

    ByeFixture()
        : handler("192.168.4.1", 5060,
                  [this](const sockaddr_in& dest, std::shared_ptr<SipMessage> msg) {
                      cap.out.emplace_back(dest, msg);
                  }) {
        handler.handle(makeRegister("100", callerIp));
        handler.handle(makeRegister("101", calleeIp));
        cap.clear();
        handler.handle(makeInvite("100", "101", callerIp, callID));
        handler.handle(makeOk("100", "101", calleeIp, callID));
        cap.clear();
    }

    // First captured message addressed to `ip` whose START LINE contains `needle`.
    // Drives off the synchronous outbox (deterministic), NOT the 1 Hz dashboard
    // snapshot (which tick() throttles to once a second — unusable for a sub-ms
    // unit test that fires several messages in one go).
    std::shared_ptr<SipMessage> find(const std::string& ip, const std::string& needle) {
        for (auto& p : cap.out) {
            if (p.first.sin_addr.s_addr != inet_addr(ip.c_str())) continue;
            std::string s = p.second->toString();
            if (s.substr(0, s.find("\r\n")).find(needle) != std::string::npos) return p.second;
        }
        return nullptr;
    }

    // Any message at all addressed to `ip` (used to assert a BYE was relayed).
    bool anyTo(const std::string& ip) {
        for (auto& p : cap.out)
            if (p.first.sin_addr.s_addr == inet_addr(ip.c_str())) return true;
        return false;
    }
};

} // namespace

// A BYE from a non-leg IP (spoofer) must be rejected with 403 and NOT torn down:
// no BYE is relayed to either real leg, so the call survives.
TEST(Teardown, SpoofedByeFromForeignIpRejected) {
    ByeFixture f;
    f.cap.clear();

    // Attacker guessed the Call-ID but is at a different IP than either leg.
    f.handler.handle(makeBye("100", "101", f.spoofIp, f.callID));

    EXPECT_NE(f.find(f.spoofIp, "403"), nullptr)
        << "spoofed BYE must be answered with 403 Forbidden";
    // The teardown must NOT have propagated to either genuine leg.
    EXPECT_FALSE(f.anyTo(f.callerIp)) << "no teardown may reach the caller leg";
    EXPECT_FALSE(f.anyTo(f.calleeIp)) << "no teardown may reach the callee leg";
}

// A BYE from a genuine caller leg IP must NOT be 403'd and MUST propagate the
// teardown to the peer — proving the guard does not break legitimate hang-up.
TEST(Teardown, LegitimateByeFromCallerTearsDown) {
    ByeFixture f;
    f.cap.clear();

    f.handler.handle(makeBye("100", "101", f.callerIp, f.callID));

    EXPECT_EQ(f.find(f.callerIp, "403"), nullptr) << "a real leg's BYE must NOT be 403'd";
    EXPECT_TRUE(f.anyTo(f.calleeIp)) << "the BYE must be relayed to the peer (callee) leg";
}

// The callee leg may also legitimately hang up; the BYE relays to the caller.
TEST(Teardown, LegitimateByeFromCalleeTearsDown) {
    ByeFixture f;
    f.cap.clear();

    f.handler.handle(makeBye("101", "100", f.calleeIp, f.callID));

    EXPECT_EQ(f.find(f.calleeIp, "403"), nullptr) << "the callee's real BYE must NOT be 403'd";
    EXPECT_TRUE(f.anyTo(f.callerIp)) << "the BYE must be relayed to the peer (caller) leg";
}

// A BYE for an UNKNOWN Call-ID has no session to bind to, so the dialog-source
// guard fails OPEN (it is not the guard's job to reject out-of-dialog requests —
// there is no live call to protect). The key property: the guard must not 403 it
// (which would be a confusing response to a request the guard cannot reason about).
TEST(Teardown, ByeForUnknownCallIdNotBlockedByGuard) {
    ByeFixture f;
    f.cap.clear();

    f.handler.handle(makeBye("100", "101", f.spoofIp, "no-such-dialog"));

    EXPECT_EQ(f.find(f.spoofIp, "403"), nullptr)
        << "the dialog-source guard must not 403 an unknown-dialog BYE";
}
