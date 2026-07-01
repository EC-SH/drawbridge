// Park_test.cpp — call parking / park-orbit (roadmap §3.1).
//
// End-to-end through RequestsHandler (harness pattern from Hold_test.cpp):
// register 100 + 101, then drive park / retrieve / timeout / teardown against the
// virtual orbit extensions 700.. and assert the on-wire exchange and the
// dashboard view (getParkedCalls):
//   * INVITE to a FREE orbit -> 200 OK echoing the parker's own SDP, slot shown
//     parked on the dashboard.
//   * INVITE to an OCCUPIED orbit -> retriever gets 200 OK with the PARKED party's
//     SDP, the parked party gets an in-dialog re-INVITE with the RETRIEVER's SDP,
//     and the orbit is released.
//   * park timeout -> the registered parker is rung back (server-as-UAC INVITE).
//   * parked party BYE -> 200 OK and the orbit is freed.

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

std::string sdpBody(const std::string& ip, uint16_t rtpPort) {
    return
        "v=0\r\n"
        "o=- 0 0 IN IP4 " + ip + "\r\n"
        "s=-\r\n"
        "c=IN IP4 " + ip + "\r\n"
        "t=0 0\r\n"
        "m=audio " + std::to_string(rtpPort) + " RTP/AVP 0 8 101\r\n"
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

// INVITE from `from` to orbit/extension `to`, carrying SDP with `rtpPort`.
std::shared_ptr<SipMessage> makeInvite(const std::string& from, const std::string& to,
                                       const std::string& srcIp, const std::string& callID,
                                       uint16_t rtpPort) {
    std::string body = sdpBody(srcIp, rtpPort);
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

std::shared_ptr<SipMessage> makeBye(const std::string& from, const std::string& to,
                                    const std::string& srcIp, const std::string& callID) {
    std::string raw =
        "BYE sip:" + to + "@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bKb\r\n"
        "From: <sip:" + from + "@server>;tag=ft" + from + "\r\n"
        "To: <sip:" + to + "@server>;tag=parktag\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 2 BYE\r\n"
        "Content-Length: 0\r\n\r\n";
    return RequestsHandler::getMessageFromPool(raw, makeAddr(srcIp));
}

struct Cap {
    std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> out;
    void clear() { out.clear(); }
};

struct ParkFixture {
    Cap cap;
    RequestsHandler handler;
    const std::string ip100 = "192.168.7.100";
    const std::string ip101 = "192.168.7.101";

    ParkFixture()
        : handler("192.168.7.1", 5060,
                  [this](const sockaddr_in& dest, std::shared_ptr<SipMessage> msg) {
                      cap.out.emplace_back(dest, msg);
                  }) {
        handler.handle(makeRegister("100", ip100));
        handler.handle(makeRegister("101", ip101));
        cap.clear();
    }

    // First captured message addressed to `ip` whose start line contains `needle`.
    std::shared_ptr<SipMessage> find(const std::string& ip, const std::string& needle) {
        for (auto& p : cap.out) {
            if (p.first.sin_addr.s_addr != inet_addr(ip.c_str())) continue;
            std::string s = p.second->toString();
            if (s.rfind(needle, 0) == 0 || s.find("\n" + needle) != std::string::npos ||
                s.substr(0, s.find("\r\n")).find(needle) != std::string::npos) {
                return p.second;
            }
        }
        return nullptr;
    }
};

uint32_t ipOf(const sockaddr_in& a) { return a.sin_addr.s_addr; }

// The full header line beginning with `name` (e.g. "From:") from a message, "".
std::string hdr(const std::shared_ptr<SipMessage>& m, const std::string& name) {
    std::string s = m->toString();
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find("\r\n", p);
        std::string line = s.substr(p, (e == std::string::npos ? s.size() : e) - p);
        if (line.rfind(name, 0) == 0) return line;
        if (e == std::string::npos) break;
        p = e + 2;
    }
    return "";
}

} // namespace

TEST(Park, InviteToFreeOrbitParksAndEchoesSdp) {
    ParkFixture f;
    f.handler.handle(makeInvite("100", "700", f.ip100, "park-1", 40000));

    // Exactly one response, a 200 OK back to the parker echoing its SDP.
    ASSERT_GE(f.cap.out.size(), 1u);
    auto ok = f.find(f.ip100, "SIP/2.0 200");
    ASSERT_NE(ok, nullptr) << "parker must get a 200 OK";
    std::string s = ok->toString();
    size_t sep = s.find("\r\n\r\n");
    ASSERT_NE(sep, std::string::npos);
    std::string body = s.substr(sep + 4);
    EXPECT_NE(body.find("m=audio"), std::string::npos) << "200 OK must echo SDP";
    EXPECT_EQ(std::string(ok->getContentLength()),
              "Content-Length: " + std::to_string(body.size()));

    // Dashboard shows the parked call on orbit 700.
    auto parked = f.handler.getParkedCalls();
    ASSERT_EQ(parked.size(), 1u);
    EXPECT_EQ(std::get<0>(parked[0]), "700");
    EXPECT_EQ(std::get<1>(parked[0]), "100");
}

TEST(Park, RetrieveBridgesPeerToPeerAndFreesOrbit) {
    ParkFixture f;
    f.handler.handle(makeInvite("100", "700", f.ip100, "park-1", 40000));
    f.cap.clear();

    // 101 retrieves the parked call by dialing the same orbit.
    f.handler.handle(makeInvite("101", "700", f.ip101, "retr-1", 41000));

    // Retriever (101) is answered 200 OK carrying the PARKED party's SDP (its RTP
    // port 40000) so 101 sends media to 100.
    auto ok = f.find(f.ip101, "SIP/2.0 200");
    ASSERT_NE(ok, nullptr) << "retriever must get a 200 OK";
    EXPECT_NE(ok->toString().find("m=audio 40000"), std::string::npos)
        << "retriever's answer carries the parked party's SDP";

    // Parked party (100) gets an in-dialog re-INVITE with the RETRIEVER's SDP
    // (port 41000) so 100 re-points media at 101.
    auto reinv = f.find(f.ip100, "INVITE");
    ASSERT_NE(reinv, nullptr) << "parked party must get a re-INVITE";
    EXPECT_NE(reinv->toString().find("m=audio 41000"), std::string::npos)
        << "re-INVITE carries the retriever's SDP";

    // Orbit released — no longer parked.
    EXPECT_TRUE(f.handler.getParkedCalls().empty());
}

TEST(Park, TimeoutRingsBackRegisteredParker) {
    ParkFixture f;
    f.handler.handle(makeInvite("100", "700", f.ip100, "park-1", 40000));
    f.cap.clear();

    f.handler.setParkTimeout(std::chrono::seconds(0));   // expire immediately
    f.handler.tick();                                    // park sweep runs

    // The parker (100) is rung back with a server-originated INVITE.
    auto rb = f.find(f.ip100, "INVITE");
    ASSERT_NE(rb, nullptr) << "expired park must ring the parker back";
    EXPECT_NE(rb->toString().find("INVITE sip:100@"), std::string::npos);
}

// #120: the park-timeout ring-back INVITE must advertise the ORBIT identity as
// caller ID — display name "Orbit 700" and URI user "**700" — so the parking
// extension's phone screen shows which orbit the call is on and the dial-back
// code, without answering.
TEST(Park, RingbackFromCarriesOrbitCallerId) {
    ParkFixture f;
    f.handler.handle(makeInvite("100", "700", f.ip100, "park-1", 40000));
    f.cap.clear();
    f.handler.setParkTimeout(std::chrono::seconds(0));
    f.handler.tick();

    auto rb = f.find(f.ip100, "INVITE");
    ASSERT_NE(rb, nullptr) << "expired park must ring the parker back";
    std::string from = hdr(rb, "From:");
    EXPECT_NE(from.find("\"Orbit 700\""), std::string::npos)
        << "ring-back From must show the orbit as the display name; got: " << from;
    EXPECT_NE(from.find("sip:**700@"), std::string::npos)
        << "ring-back From URI user must be the **700 dial-back code; got: " << from;
}

// #120: a caller dialing the caller-ID number literally — "**700" — retrieves
// the parked call exactly as the bare orbit "700" does.
TEST(Park, StarStarOrbitRetrievesLikeBareOrbit) {
    ParkFixture f;
    f.handler.handle(makeInvite("100", "700", f.ip100, "park-1", 40000));
    f.cap.clear();

    // 101 retrieves by dialing the **-prefixed alias shown on the phone screen.
    f.handler.handle(makeInvite("101", "**700", f.ip101, "retr-1", 41000));

    // Retriever is answered with the parked party's SDP (port 40000) — bridged.
    auto ok = f.find(f.ip101, "SIP/2.0 200");
    ASSERT_NE(ok, nullptr) << "**700 must retrieve the parked call (200 OK to retriever)";
    EXPECT_NE(ok->toString().find("m=audio 40000"), std::string::npos)
        << "retriever's answer carries the parked party's SDP";

    // Parked party gets the re-INVITE with the retriever's SDP (port 41000).
    auto reinv = f.find(f.ip100, "INVITE");
    ASSERT_NE(reinv, nullptr) << "parked party must get a re-INVITE on **700 retrieve";
    EXPECT_NE(reinv->toString().find("m=audio 41000"), std::string::npos);

    // Orbit released.
    EXPECT_TRUE(f.handler.getParkedCalls().empty());
}

// #120 regression guard: dialing "**700" while the orbit is FREE parks the
// caller there (same as bare "700"), rather than 404ing or leaking to a trunk.
TEST(Park, StarStarOrbitToFreeOrbitParks) {
    ParkFixture f;
    f.handler.handle(makeInvite("100", "**700", f.ip100, "park-1", 40000));

    auto ok = f.find(f.ip100, "SIP/2.0 200");
    ASSERT_NE(ok, nullptr) << "**700 to a free orbit must park (200 OK to parker)";
    auto parked = f.handler.getParkedCalls();
    ASSERT_EQ(parked.size(), 1u);
    EXPECT_EQ(std::get<0>(parked[0]), "700") << "slot must record the canonical bare orbit";
}

TEST(Park, ParkedPartyByeFreesOrbit) {
    ParkFixture f;
    f.handler.handle(makeInvite("100", "700", f.ip100, "park-1", 40000));
    ASSERT_EQ(f.handler.getParkedCalls().size(), 1u);
    f.cap.clear();

    f.handler.handle(makeBye("100", "700", f.ip100, "park-1"));

    auto ok = f.find(f.ip100, "SIP/2.0 200");
    EXPECT_NE(ok, nullptr) << "BYE must be 200 OK'd";
    EXPECT_TRUE(f.handler.getParkedCalls().empty()) << "orbit freed on parked-party BYE";
}

TEST(Park, NoParkedCallsInitially) {
    ParkFixture f;
    EXPECT_TRUE(f.handler.getParkedCalls().empty());
}

// Regression (ultrareview bug_002, case a): the ACK the server sends to confirm
// the parked party's 200 OK to the retrieve re-INVITE must not double the
// From:/To: header names (getFrom()/getTo() already include them).
TEST(Park, RetrieveReinviteAckIsWellFormed) {
    ParkFixture f;
    f.handler.handle(makeInvite("100", "700", f.ip100, "park-1", 40000));
    f.handler.handle(makeInvite("101", "700", f.ip101, "retr-1", 41000));
    auto reinv = f.find(f.ip100, "INVITE");           // re-INVITE the server sent the parked party
    ASSERT_NE(reinv, nullptr);
    f.cap.clear();

    // 100 answers the re-INVITE 200 OK (From/To copied from the request verbatim).
    std::string ok =
        "SIP/2.0 200 OK\r\n"
        "Via: SIP/2.0/UDP " + f.ip100 + ":5060;branch=z9hG4bKp\r\n" +
        hdr(reinv, "From:") + "\r\n" + hdr(reinv, "To:") + "\r\n" + hdr(reinv, "Call-ID:") + "\r\n"
        "CSeq: 2 INVITE\r\n"
        "Content-Length: 0\r\n\r\n";
    f.handler.handle(RequestsHandler::getMessageFromPool(ok, makeAddr(f.ip100)));

    auto ack = f.find(f.ip100, "ACK");
    ASSERT_NE(ack, nullptr) << "server must ACK the parked party's re-INVITE 200 OK";
    std::string s = ack->toString();
    EXPECT_EQ(s.find("From: From:"), std::string::npos) << "ACK must not double the From: name";
    EXPECT_EQ(s.find("To: To:"), std::string::npos) << "ACK must not double the To: name";
}

// Regression (ultrareview bug_001 + bug_002 case b): when the parker answers a
// park-timeout ring-back, the ACK is well-formed AND the two legs are linked in
// _sessions, so a BYE on the ring-back dialog is relayed to the parked party.
TEST(Park, RingbackAnswerAcksAndLinksBridge) {
    ParkFixture f;
    f.handler.handle(makeInvite("100", "700", f.ip100, "park-1", 40000));
    f.handler.setParkTimeout(std::chrono::seconds(0));
    f.handler.tick();                                  // ring-back INVITE to the parker (100)
    auto rb = f.find(f.ip100, "INVITE");
    ASSERT_NE(rb, nullptr);
    std::string rbCallId = hdr(rb, "Call-ID:");
    ASSERT_FALSE(rbCallId.empty());
    f.cap.clear();

    // 100 answers the ring-back 200 OK (To gains the answerer's tag), with SDP.
    std::string body = sdpBody(f.ip100, 42000);
    std::string ok =
        "SIP/2.0 200 OK\r\n"
        "Via: SIP/2.0/UDP " + f.ip100 + ":5060;branch=z9hG4bKrb\r\n" +
        hdr(rb, "From:") + "\r\n" + hdr(rb, "To:") + ";tag=rbanswer\r\n" + rbCallId + "\r\n"
        "CSeq: 1 INVITE\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    f.handler.handle(RequestsHandler::getMessageFromPool(ok, makeAddr(f.ip100)));

    auto ack = f.find(f.ip100, "ACK");
    ASSERT_NE(ack, nullptr) << "server must ACK the ring-back 200 OK";
    EXPECT_EQ(ack->toString().find("To: To:"), std::string::npos) << "ring-back ACK must not double To:";

    // Bridge is linked: a BYE on the ring-back dialog relays a server BYE to the
    // parked leg (without the _sessions link the peer would be orphaned).
    f.cap.clear();
    std::string callIdVal = rbCallId.substr(rbCallId.find(':') + 2);   // strip "Call-ID: "
    std::string bye =
        "BYE sip:700@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + f.ip100 + ":5060;branch=z9hG4bKbye\r\n"
        "From: <sip:100@server>;tag=rbanswer\r\n"
        "To: <sip:700@server>;tag=parktag\r\n"
        "Call-ID: " + callIdVal + "\r\n"
        "CSeq: 2 BYE\r\n"
        "Content-Length: 0\r\n\r\n";
    f.handler.handle(RequestsHandler::getMessageFromPool(bye, makeAddr(f.ip100)));
    EXPECT_NE(f.find(f.ip100, "BYE"), nullptr) << "BYE on the bridged leg must relay to the peer";
}
