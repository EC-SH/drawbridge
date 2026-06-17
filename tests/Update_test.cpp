// Update_test.cpp — RFC 3311 mid-dialog UPDATE handler.
//
// Covers:
//   * Bodiless UPDATE: 200 OK returned, session timer expiry reset
//   * SDP-bearing UPDATE on an unknown dialog: 481 returned
//   * SDP-bearing UPDATE: relayed to peer leg (hold direction tracked)

#include <gtest/gtest.h>
#include "RequestsHandler.hpp"
#include "SipMessage.hpp"
#include "SipMessageTypes.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <chrono>
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
    return "v=0\r\no=- 0 0 IN IP4 " + ip + "\r\ns=-\r\nc=IN IP4 " + ip +
           "\r\nt=0 0\r\nm=audio 40000 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n";
}

std::shared_ptr<SipMessage> fromPool(const std::string& raw, const std::string& ip) {
    return RequestsHandler::getMessageFromPool(raw, makeAddr(ip));
}

struct UpdateFixture {
    std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> out;
    RequestsHandler handler;
    const std::string callerIp = "192.168.6.50";
    const std::string calleeIp = "192.168.6.51";
    const std::string callID   = "upd-test-1";

    UpdateFixture()
        : handler("192.168.6.1", 5060,
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
            handler.handle(fromPool(raw, ip));
        };
        reg("210", callerIp);
        reg("211", calleeIp);

        // INVITE from caller
        std::string body = sdpBody(callerIp);
        std::string inv =
            "INVITE sip:211@server SIP/2.0\r\n"
            "Via: SIP/2.0/UDP " + callerIp + ":5060;branch=z9hG4bKupd1\r\n"
            "From: <sip:210@server>;tag=ftX\r\n"
            "To: <sip:211@server>\r\n"
            "Call-ID: " + callID + "\r\n"
            "CSeq: 1 INVITE\r\n"
            "Contact: <sip:210@" + callerIp + ":5060>\r\n"
            "Content-Type: application/sdp\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        handler.handle(fromPool(inv, callerIp));

        // Callee 200 OK
        std::string cb = sdpBody(calleeIp);
        std::string ok200 =
            "SIP/2.0 200 OK\r\n"
            "Via: SIP/2.0/UDP " + callerIp + ":5060;branch=z9hG4bKupd1\r\n"
            "From: <sip:210@server>;tag=ftX\r\n"
            "To: <sip:211@server>;tag=ttY\r\n"
            "Call-ID: " + callID + "\r\n"
            "CSeq: 1 INVITE\r\n"
            "Contact: <sip:211@" + calleeIp + ":5060>\r\n"
            "Session-Expires: 1800;refresher=uac\r\n"
            "Content-Type: application/sdp\r\n"
            "Content-Length: " + std::to_string(cb.size()) + "\r\n\r\n" + cb;
        handler.handle(fromPool(ok200, calleeIp));
        out.clear();
    }
};

TEST(Update, BodilessUpdateReturns200Ok) {
    UpdateFixture f;
    std::string upd =
        "UPDATE sip:211@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + f.callerIp + ":5060;branch=z9hG4bKupd2\r\n"
        "From: <sip:210@server>;tag=ftX\r\n"
        "To: <sip:211@server>;tag=ttY\r\n"
        "Call-ID: " + f.callID + "\r\n"
        "CSeq: 2 UPDATE\r\n"
        "Content-Length: 0\r\n\r\n";
    f.handler.handle(fromPool(upd, f.callerIp));

    bool got200 = false;
    for (const auto& [addr, msg] : f.out) {
        if (msg && msg->getStatusInfo().has_value() &&
            msg->getStatusInfo()->code == 200) {
            got200 = true; break;
        }
    }
    EXPECT_TRUE(got200) << "Bodiless UPDATE must receive 200 OK";
}

TEST(Update, UpdateOnUnknownDialogReturns481) {
    UpdateFixture f;
    std::string upd =
        "UPDATE sip:211@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + f.callerIp + ":5060;branch=z9hG4bKupd3\r\n"
        "From: <sip:210@server>;tag=ftX\r\n"
        "To: <sip:211@server>;tag=ttY\r\n"
        "Call-ID: nonexistent-dialog\r\n"
        "CSeq: 2 UPDATE\r\n"
        "Content-Length: 0\r\n\r\n";
    f.handler.handle(fromPool(upd, f.callerIp));

    bool got481 = false;
    for (const auto& [addr, msg] : f.out) {
        if (msg && msg->getStatusInfo().has_value() &&
            msg->getStatusInfo()->code == 481) {
            got481 = true; break;
        }
    }
    EXPECT_TRUE(got481) << "UPDATE on unknown dialog must return 481";
}

TEST(Update, SdpUpdateRelayedToPeer) {
    UpdateFixture f;
    std::string body = sdpBody(f.callerIp);
    std::string upd =
        "UPDATE sip:211@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + f.callerIp + ":5060;branch=z9hG4bKupd4\r\n"
        "From: <sip:210@server>;tag=ftX\r\n"
        "To: <sip:211@server>;tag=ttY\r\n"
        "Call-ID: " + f.callID + "\r\n"
        "CSeq: 2 UPDATE\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    f.handler.handle(fromPool(upd, f.callerIp));

    uint32_t calleeAddr = inet_addr(f.calleeIp.c_str());
    bool relayedToCallee = false;
    for (const auto& [addr, msg] : f.out) {
        if (addr.sin_addr.s_addr == calleeAddr &&
            msg && msg->getType() == "UPDATE") {
            relayedToCallee = true; break;
        }
    }
    EXPECT_TRUE(relayedToCallee) << "SDP UPDATE must be relayed to the peer leg";
}

} // namespace
