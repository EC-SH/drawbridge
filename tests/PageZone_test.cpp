// PageZone_test.cpp — integration coverage for the multi-zone paging feature
// through the real RequestsHandler: the thread-safe setPageZone/getPageZones
// mutators, the 98x reservation in setRingGroup/setForward, and the zone-INVITE
// fan-out (which reuses the 999 startBroadcastFork machinery with intercom
// auto-answer headers).

#include <gtest/gtest.h>
#include "RequestsHandler.hpp"
#include "SipMessage.hpp"

#if defined(_WIN32) || defined(_WIN64)
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#endif

#include <memory>
#include <string>
#include <vector>

namespace {

struct Captured {
    std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> out;
};

sockaddr_in addr(const char* ip, uint16_t port) {
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = inet_addr(ip);
    s.sin_port = htons(port);
    return s;
}

std::shared_ptr<SipMessage> makeRegister(const std::string& ext, const char* srcIp,
                                         const std::string& callId) {
    sockaddr_in s = addr(srcIp, 5060);
    std::string raw =
        "REGISTER sip:192.168.4.1 SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + std::string(srcIp) + ":5060;branch=z9hG4bKr" + ext + "\r\n"
        "From: <sip:" + ext + "@192.168.4.1>;tag=ft" + ext + "\r\n"
        "To: <sip:" + ext + "@192.168.4.1>\r\n"
        "Call-ID: " + callId + "\r\n"
        "CSeq: 1 REGISTER\r\n"
        "Contact: <sip:" + ext + "@" + srcIp + ":5060>;expires=3600\r\n"
        "Content-Length: 0\r\n\r\n";
    return RequestsHandler::getMessageFromPool(raw, s);
}

std::shared_ptr<SipMessage> makeInvite(const std::string& from, const std::string& to,
                                       const char* srcIp, const std::string& callId) {
    sockaddr_in s = addr(srcIp, 5060);
    std::string body =
        "v=0\r\n"
        "o=- 1 1 IN IP4 " + std::string(srcIp) + "\r\n"
        "s=-\r\n"
        "c=IN IP4 " + std::string(srcIp) + "\r\n"
        "t=0 0\r\n"
        "m=audio 4000 RTP/AVP 0 8 101\r\n"
        "a=rtpmap:0 PCMU/8000\r\n";
    std::string raw =
        "INVITE sip:" + to + "@192.168.4.1 SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + std::string(srcIp) + ":5060;branch=z9hG4bKi" + callId + "\r\n"
        "From: <sip:" + from + "@192.168.4.1>;tag=ft" + from + "\r\n"
        "To: <sip:" + to + "@192.168.4.1>\r\n"
        "Call-ID: " + callId + "\r\n"
        "CSeq: 1 INVITE\r\n"
        "Contact: <sip:" + from + "@" + srcIp + ":5060>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    return RequestsHandler::getMessageFromPool(raw, s);
}

}   // namespace

TEST(PageZoneHandler, SetAndGetRoundTripsDedupedMembers) {
    Captured cap;
    RequestsHandler handler("192.168.4.1", 5060,
        [&cap](const sockaddr_in& d, std::shared_ptr<SipMessage> m) {
            cap.out.emplace_back(d, m);
        });

    handler.setPageZone("985", "101, 102, 101");
    auto zones = handler.getPageZones();
    ASSERT_EQ(zones.size(), 1u);
    EXPECT_EQ(zones[0].first, "985");
    EXPECT_EQ(zones[0].second, "101,102");

    // Empty membership deletes the zone.
    handler.setPageZone("985", "");
    EXPECT_TRUE(handler.getPageZones().empty());
}

TEST(PageZoneHandler, NonZoneExtensionRejected) {
    Captured cap;
    RequestsHandler handler("192.168.4.1", 5060,
        [&cap](const sockaddr_in& d, std::shared_ptr<SipMessage> m) {
            cap.out.emplace_back(d, m);
        });

    handler.setPageZone("600", "101,102");   // not in 980-989
    handler.setPageZone("999", "101,102");   // the all-page is not a zone
    EXPECT_TRUE(handler.getPageZones().empty());
}

TEST(PageZoneHandler, ZoneRangeReservedFromRingGroupsAndForwards) {
    Captured cap;
    RequestsHandler handler("192.168.4.1", 5060,
        [&cap](const sockaddr_in& d, std::shared_ptr<SipMessage> m) {
            cap.out.emplace_back(d, m);
        });

    handler.setRingGroup("983", "101,102", "ringall");
    EXPECT_TRUE(handler.getRingGroups().empty());

    handler.setForward("983", "always", "200");
    EXPECT_TRUE(handler.getForwards().empty());

    // Sanity: a non-reserved extension still configures normally.
    handler.setRingGroup("600", "101,102", "ringall");
    EXPECT_EQ(handler.getRingGroups().size(), 1u);
}

TEST(PageZoneHandler, ZoneInviteForksIntercomToRegisteredMembersOnly) {
    Captured cap;
    RequestsHandler handler("192.168.4.1", 5060,
        [&cap](const sockaddr_in& d, std::shared_ptr<SipMessage> m) {
            cap.out.emplace_back(d, m);
        });

    handler.handle(makeRegister("100", "192.168.4.50", "reg-z-100"));
    handler.handle(makeRegister("101", "192.168.4.51", "reg-z-101"));
    handler.handle(makeRegister("102", "192.168.4.52", "reg-z-102"));
    // 103 is a zone member but NOT registered -> must be skipped, not forked.
    handler.setPageZone("981", "100,101,102,103");
    cap.out.clear();

    handler.handle(makeInvite("100", "981", "192.168.4.50", "page-1"));

    // Caller hears 180 Ringing; the dialog Contact carries the zone extension.
    bool sawRinging = false;
    int inviteForks = 0;
    for (auto& e : cap.out) {
        std::string raw = e.second->toString();
        auto st = e.second->getStatusInfo();
        if (st.has_value() && st->code == 180) {
            sawRinging = true;
            EXPECT_NE(raw.find("sip:981@"), std::string::npos)
                << "180 Contact should carry the zone extension";
        }
        if (raw.rfind("INVITE ", 0) == 0) {
            ++inviteForks;
            // Zone pages are intercom: the auto-answer header set must be present.
            EXPECT_NE(raw.find("answer-after=0"), std::string::npos);
            // The caller (100) and the offline member (103) must not be forked.
            EXPECT_EQ(raw.find("INVITE sip:100@"), std::string::npos);
            EXPECT_EQ(raw.find("INVITE sip:103@"), std::string::npos);
        }
    }
    EXPECT_TRUE(sawRinging);
    EXPECT_EQ(inviteForks, 2) << "expected one fork per registered member (101, 102)";
}

TEST(PageZoneHandler, ZoneWithNoRegisteredMembersIs480) {
    Captured cap;
    RequestsHandler handler("192.168.4.1", 5060,
        [&cap](const sockaddr_in& d, std::shared_ptr<SipMessage> m) {
            cap.out.emplace_back(d, m);
        });

    handler.handle(makeRegister("100", "192.168.4.50", "reg-e-100"));
    handler.setPageZone("982", "300,301");   // nobody home
    cap.out.clear();

    handler.handle(makeInvite("100", "982", "192.168.4.50", "page-empty"));

    bool saw480 = false;
    for (auto& e : cap.out) {
        auto st = e.second->getStatusInfo();
        if (st.has_value() && st->code == 480) saw480 = true;
    }
    EXPECT_TRUE(saw480) << "empty zone should answer 480 Temporarily Unavailable";
}
