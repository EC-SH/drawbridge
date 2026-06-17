// SessionTimer_test.cpp — RFC 4028 session timer parsing and sweep.
//
// Covers:
//   * getSessionExpiresSecs(): numeric value extraction
//   * getSessionExpiresRefresher(): "uac"/"uas"/empty
//   * getMinSESecs(): Min-SE numeric value
//   * armSessionTimer(): wires Session fields from a 200 OK
//   * sweepSessionTimers(): sends BYE when session timer fires

#include <gtest/gtest.h>
#include "RequestsHandler.hpp"
#include "SipMessage.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <chrono>
#include <string>
#include <thread>
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
    return "v=0\r\no=- 0 0 IN IP4 " + ip + "\r\ns=-\r\nc=IN IP4 " + ip +
           "\r\nt=0 0\r\nm=audio 40000 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n";
}

std::shared_ptr<SipMessage> makeMsg(const std::string& raw, const std::string& ip = "1.2.3.4") {
    return RequestsHandler::getMessageFromPool(raw, makeAddr(ip));
}

// ── SipMessage header parsing ────────────────────────────────────────────────

TEST(SessionTimerHeaders, GetSessionExpiresSecsBasic) {
    auto m = makeMsg(
        "INVITE sip:101@s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 1.2.3.4:5060;branch=z9hG4bKx\r\n"
        "From: <sip:100@s>;tag=a\r\n"
        "To: <sip:101@s>\r\n"
        "Call-ID: cid\r\n"
        "CSeq: 1 INVITE\r\n"
        "Session-Expires: 1800\r\n"
        "Min-SE: 90\r\n"
        "Content-Length: 0\r\n\r\n");
    EXPECT_EQ(m->getSessionExpiresSecs(), 1800u);
    EXPECT_EQ(m->getMinSESecs(),          90u);
    EXPECT_TRUE(m->getSessionExpiresRefresher().empty());
}

TEST(SessionTimerHeaders, GetSessionExpiresRefresherUac) {
    auto m = makeMsg(
        "SIP/2.0 200 OK\r\n"
        "Via: SIP/2.0/UDP 1.2.3.4:5060;branch=z9hG4bKx\r\n"
        "From: <sip:100@s>;tag=a\r\n"
        "To: <sip:101@s>;tag=b\r\n"
        "Call-ID: cid\r\n"
        "CSeq: 1 INVITE\r\n"
        "Session-Expires: 1800;refresher=uac\r\n"
        "Content-Length: 0\r\n\r\n");
    EXPECT_EQ(m->getSessionExpiresSecs(), 1800u);
    EXPECT_EQ(m->getSessionExpiresRefresher(), "uac");
}

TEST(SessionTimerHeaders, GetSessionExpiresRefresherUas) {
    auto m = makeMsg(
        "SIP/2.0 200 OK\r\n"
        "Via: SIP/2.0/UDP 1.2.3.4:5060;branch=z9hG4bKx\r\n"
        "From: <sip:100@s>;tag=a\r\n"
        "To: <sip:101@s>;tag=b\r\n"
        "Call-ID: cid\r\n"
        "CSeq: 1 INVITE\r\n"
        "Session-Expires: 900;refresher=uas\r\n"
        "Content-Length: 0\r\n\r\n");
    EXPECT_EQ(m->getSessionExpiresSecs(), 900u);
    EXPECT_EQ(m->getSessionExpiresRefresher(), "uas");
}

TEST(SessionTimerHeaders, AbsentHeadersReturnZeroAndEmpty) {
    auto m = makeMsg(
        "INVITE sip:101@s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 1.2.3.4:5060;branch=z9hG4bKx\r\n"
        "From: <sip:100@s>;tag=a\r\n"
        "To: <sip:101@s>\r\n"
        "Call-ID: cid\r\n"
        "CSeq: 1 INVITE\r\n"
        "Content-Length: 0\r\n\r\n");
    EXPECT_EQ(m->getSessionExpiresSecs(), 0u);
    EXPECT_EQ(m->getMinSESecs(),          0u);
    EXPECT_TRUE(m->getSessionExpiresRefresher().empty());
}

TEST(SessionTimerHeaders, SessionExpiresNotParsedFromBody) {
    // Ensure Session-Expires after \r\n\r\n (in body) is ignored.
    auto m = makeMsg(
        "SIP/2.0 200 OK\r\n"
        "Via: SIP/2.0/UDP 1.2.3.4:5060;branch=z9hG4bKx\r\n"
        "From: <sip:100@s>;tag=a\r\n"
        "To: <sip:101@s>;tag=b\r\n"
        "Call-ID: cid\r\n"
        "CSeq: 1 INVITE\r\n"
        "Content-Length: 18\r\n\r\n"
        "Session-Expires: 999");
    EXPECT_EQ(m->getSessionExpiresSecs(), 0u);
}

// ── Integration: arm + sweep ─────────────────────────────────────────────────

struct TimerFixture {
    std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> out;
    RequestsHandler handler;
    const std::string callerIp = "192.168.5.50";
    const std::string calleeIp = "192.168.5.51";

    TimerFixture()
        : handler("192.168.5.1", 5060,
                  [this](const sockaddr_in& d, std::shared_ptr<SipMessage> m) {
                      out.emplace_back(d, std::move(m));
                  }) {
        auto reg = [&](const std::string& ext, const std::string& ip) {
            std::string raw =
                "REGISTER sip:server SIP/2.0\r\n"
                "Via: SIP/2.0/UDP " + ip + ":5060;branch=z9hG4bKr" + ext + "\r\n"
                "From: <sip:" + ext + "@server>;tag=rt" + ext + "\r\n"
                "To: <sip:" + ext + "@server>\r\n"
                "Call-ID: reg-" + ext + "\r\n"
                "CSeq: 1 REGISTER\r\n"
                "Contact: <sip:" + ext + "@" + ip + ":5060>;expires=3600\r\n"
                "Content-Length: 0\r\n\r\n";
            handler.handle(RequestsHandler::getMessageFromPool(raw, makeAddr(ip)));
        };
        reg("200", callerIp);
        reg("201", calleeIp);
        out.clear();
    }
};

// Connect a call whose 200 OK carries Session-Expires;refresher=uac.
// The session timer should be armed but isRefresher must be false (UAC refreshes).
TEST(SessionTimer, ArmFromOk200RefresherUac) {
    TimerFixture f;
    const std::string callID = "st-arm-1";
    const std::string branch = "z9hG4bKst1";
    std::string body = sdpBody(f.callerIp);
    std::string inv =
        "INVITE sip:201@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + f.callerIp + ":5060;branch=" + branch + "\r\n"
        "From: <sip:200@server>;tag=ftA\r\n"
        "To: <sip:201@server>\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 1 INVITE\r\n"
        "Contact: <sip:200@" + f.callerIp + ":5060>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    f.handler.handle(RequestsHandler::getMessageFromPool(inv, makeAddr(f.callerIp)));
    f.out.clear();

    // Callee answers with 200 OK that includes Session-Expires;refresher=uac
    std::string ok200 =
        "SIP/2.0 200 OK\r\n"
        "Via: SIP/2.0/UDP " + f.callerIp + ":5060;branch=" + branch + "\r\n"
        "From: <sip:200@server>;tag=ftA\r\n"
        "To: <sip:201@server>;tag=ttB\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 1 INVITE\r\n"
        "Contact: <sip:201@" + f.calleeIp + ":5060>\r\n"
        "Session-Expires: 1800;refresher=uac\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(sdpBody(f.calleeIp).size()) + "\r\n\r\n" +
        sdpBody(f.calleeIp);
    f.handler.handle(RequestsHandler::getMessageFromPool(ok200, makeAddr(f.calleeIp)));

    // The session is now Connected.  A tick() well before T/2 must NOT send a BYE.
    f.out.clear();
    f.handler.tick();
    // No BYE in output — session not expired.
    for (const auto& [addr, msg] : f.out) {
        EXPECT_NE(msg->getType(), "BYE")
            << "No BYE should be sent before session timer expires";
    }
}

// Session whose timer (2 s) fires: sweepSessionTimers must send BYE to both legs.
TEST(SessionTimer, ExpiredSessionGetsBye) {
    TimerFixture f;
    const std::string callID = "st-expire-1";
    const std::string branch = "z9hG4bKst2";
    std::string body = sdpBody(f.callerIp);
    std::string inv =
        "INVITE sip:201@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + f.callerIp + ":5060;branch=" + branch + "\r\n"
        "From: <sip:200@server>;tag=ftC\r\n"
        "To: <sip:201@server>\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 1 INVITE\r\n"
        "Contact: <sip:200@" + f.callerIp + ":5060>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    f.handler.handle(RequestsHandler::getMessageFromPool(inv, makeAddr(f.callerIp)));
    f.out.clear();

    // Answer with a 2-second session timer (tiny value for test speed).
    std::string calleeBody = sdpBody(f.calleeIp);
    std::string ok200 =
        "SIP/2.0 200 OK\r\n"
        "Via: SIP/2.0/UDP " + f.callerIp + ":5060;branch=" + branch + "\r\n"
        "From: <sip:200@server>;tag=ftC\r\n"
        "To: <sip:201@server>;tag=ttD\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 1 INVITE\r\n"
        "Contact: <sip:201@" + f.calleeIp + ":5060>\r\n"
        "Session-Expires: 2;refresher=uac\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(calleeBody.size()) + "\r\n\r\n" + calleeBody;
    f.handler.handle(RequestsHandler::getMessageFromPool(ok200, makeAddr(f.calleeIp)));
    f.out.clear();

    // Sleep past the 2-second session expiry + 1-second tick gate.
    std::this_thread::sleep_for(std::chrono::milliseconds(3200));
    f.handler.tick();

    // Expect at least one BYE in the output.
    bool foundBye = false;
    for (const auto& [addr, msg] : f.out) {
        if (msg && msg->getType() == "BYE") { foundBye = true; break; }
    }
    EXPECT_TRUE(foundBye) << "sweepSessionTimers must send BYE when timer expires";
}

// Regression: a refresh re-INVITE from either leg must reset _sessionExpiry.
// Without the fix, sweepSessionTimers() would BYE a healthy call at the original
// expiry even though the phone just sent a refresh.
TEST(SessionTimer, ReinviteRefreshExtendsTimer) {
    TimerFixture f;
    const std::string callID = "st-reinv-1";
    const std::string branch = "z9hG4bKst3";
    std::string body = sdpBody(f.callerIp);

    // Establish a call with a 2-second session timer (refresher=uac).
    std::string inv =
        "INVITE sip:201@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + f.callerIp + ":5060;branch=" + branch + "\r\n"
        "From: <sip:200@server>;tag=ftE\r\n"
        "To: <sip:201@server>\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 1 INVITE\r\n"
        "Contact: <sip:200@" + f.callerIp + ":5060>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    f.handler.handle(RequestsHandler::getMessageFromPool(inv, makeAddr(f.callerIp)));
    f.out.clear();

    std::string calleeBody = sdpBody(f.calleeIp);
    std::string ok200 =
        "SIP/2.0 200 OK\r\n"
        "Via: SIP/2.0/UDP " + f.callerIp + ":5060;branch=" + branch + "\r\n"
        "From: <sip:200@server>;tag=ftE\r\n"
        "To: <sip:201@server>;tag=ttF\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 1 INVITE\r\n"
        "Contact: <sip:201@" + f.calleeIp + ":5060>\r\n"
        "Session-Expires: 2;refresher=uac\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(calleeBody.size()) + "\r\n\r\n" + calleeBody;
    f.handler.handle(RequestsHandler::getMessageFromPool(ok200, makeAddr(f.calleeIp)));
    f.out.clear();

    // Sleep past the half-interval (1 s) — the UAC sends a refresh re-INVITE.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    std::string rbody = sdpBody(f.callerIp);
    std::string reinv =
        "INVITE sip:201@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + f.callerIp + ":5060;branch=z9hG4bKst3r\r\n"
        "From: <sip:200@server>;tag=ftE\r\n"
        "To: <sip:201@server>;tag=ttF\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 2 INVITE\r\n"
        "Contact: <sip:200@" + f.callerIp + ":5060>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(rbody.size()) + "\r\n\r\n" + rbody;
    f.handler.handle(RequestsHandler::getMessageFromPool(reinv, makeAddr(f.callerIp)));
    f.out.clear();

    // Sleep past the ORIGINAL 2-second expiry (total ~2.3 s since arm).
    // The refresh re-INVITE should have reset _sessionExpiry, so no BYE yet.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    f.handler.tick();

    for (const auto& [addr, msg] : f.out) {
        EXPECT_NE(msg->getType(), "BYE")
            << "re-INVITE refresh must extend session timer — no BYE at original expiry";
    }
}

} // namespace
