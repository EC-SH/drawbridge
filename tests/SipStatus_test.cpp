#include <gtest/gtest.h>
#include <string>
#include <type_traits>
#include "SipStatus.hpp"
#include "SipMessage.hpp"
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

using PocketDial::parseSipStatusLine;
using PocketDial::sipStatusClass;
using PocketDial::SipStatusClass;
using PocketDial::SipStatusInfo;

namespace {

sockaddr_in makeAddr()
{
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = inet_addr("127.0.0.1");
    return s;
}

// Wrap a status line in an otherwise well-formed response.
std::string response(const char* statusLine)
{
    return std::string(statusLine) + "\r\n"
        "Via: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bK1\r\n"
        "From: <sip:100@server>;tag=a\r\n"
        "To: <sip:200@server>;tag=b\r\n"
        "Call-ID: call-abc\r\n"
        "CSeq: 1 INVITE\r\n"
        "Content-Length: 0\r\n\r\n";
}

} // namespace

// ---- parseSipStatusLine: well-formed responses ----
TEST(SipStatus, ParsesWellFormedResponses)
{
    auto ok = parseSipStatusLine("SIP/2.0 200 OK\r\n");
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->code, 200u);
    EXPECT_EQ(ok->klass, SipStatusClass::Success);
    EXPECT_FALSE(ok->softFail);

    auto ringing = parseSipStatusLine("SIP/2.0 180 Ringing");
    ASSERT_TRUE(ringing.has_value());
    EXPECT_EQ(ringing->code, 180u);
    EXPECT_EQ(ringing->klass, SipStatusClass::Provisional);

    auto busy = parseSipStatusLine("SIP/2.0 486 Busy Here");
    ASSERT_TRUE(busy.has_value());
    EXPECT_EQ(busy->code, 486u);
    EXPECT_EQ(busy->klass, SipStatusClass::ClientError);
    EXPECT_TRUE(busy->softFail);            // 486 is transient/soft per the ROM table

    auto badReq = parseSipStatusLine("SIP/2.0 400 Bad Request");
    ASSERT_TRUE(badReq.has_value());
    EXPECT_EQ(badReq->code, 400u);
    EXPECT_EQ(badReq->klass, SipStatusClass::ClientError);
    EXPECT_FALSE(badReq->softFail);         // 400 is a hard failure
}

// ---- parseSipStatusLine: rejects anything that is not a SIP status line ----
TEST(SipStatus, RejectsRequestsAndJunk)
{
    EXPECT_FALSE(parseSipStatusLine("INVITE sip:bob@host SIP/2.0").has_value());
    EXPECT_FALSE(parseSipStatusLine("REGISTER sip:server SIP/2.0").has_value());
    EXPECT_FALSE(parseSipStatusLine("").has_value());
    EXPECT_FALSE(parseSipStatusLine("garbage").has_value());
    EXPECT_FALSE(parseSipStatusLine("SIP/2.0 99 TooLow").has_value());   // < 100
    EXPECT_FALSE(parseSipStatusLine("SIP/2.0 700 TooHigh").has_value()); // > 699
}

// ---- The core win: the numeric code is independent of the reason phrase ----
TEST(SipStatus, ReasonPhraseDoesNotAffectCode)
{
    auto canonical = parseSipStatusLine("SIP/2.0 486 Busy Here");
    auto variant   = parseSipStatusLine("SIP/2.0 486 Busy");
    auto noReason   = parseSipStatusLine("SIP/2.0 486");
    ASSERT_TRUE(canonical.has_value());
    ASSERT_TRUE(variant.has_value());
    ASSERT_TRUE(noReason.has_value());
    EXPECT_EQ(canonical->code, 486u);
    EXPECT_EQ(variant->code, 486u);
    EXPECT_EQ(noReason->code, 486u);
}

// ---- sipStatusClass boundaries ----
TEST(SipStatus, ClassBoundaries)
{
    EXPECT_EQ(sipStatusClass(100), SipStatusClass::Provisional);
    EXPECT_EQ(sipStatusClass(199), SipStatusClass::Provisional);
    EXPECT_EQ(sipStatusClass(200), SipStatusClass::Success);
    EXPECT_EQ(sipStatusClass(302), SipStatusClass::Redirection);
    EXPECT_EQ(sipStatusClass(404), SipStatusClass::ClientError);
    EXPECT_EQ(sipStatusClass(503), SipStatusClass::ServerError);
    EXPECT_EQ(sipStatusClass(603), SipStatusClass::GlobalFail);
    EXPECT_EQ(sipStatusClass(42),  SipStatusClass::Unknown);
    EXPECT_EQ(sipStatusClass(99),  SipStatusClass::Unknown);
}

// ---- SipStatusInfo stays a trivially-copyable POD (design guarantee) ----
TEST(SipStatus, IsTriviallyCopyablePod)
{
    EXPECT_TRUE(std::is_trivially_copyable<SipStatusInfo>::value);
    EXPECT_TRUE(std::is_standard_layout<SipStatusInfo>::value);
}

// ---- SipMessage integration: getStatusInfo() populated for a response ----
TEST(SipStatus, SipMessagePopulatesStatusInfoForResponse)
{
    SipMessage m(response("SIP/2.0 486 Busy Here"), makeAddr());
    auto info = m.getStatusInfo();
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->code, 486u);
    EXPECT_EQ(info->klass, SipStatusClass::ClientError);
    EXPECT_TRUE(info->softFail);
}

// ---- SipMessage integration: requests carry no status info ----
TEST(SipStatus, SipMessageNoStatusInfoForRequest)
{
    std::string req =
        "REGISTER sip:server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bK1\r\n"
        "From: <sip:100@server>\r\n"
        "To: <sip:100@server>\r\n"
        "Call-ID: id\r\n"
        "CSeq: 1 REGISTER\r\n"
        "Content-Length: 0\r\n\r\n";
    SipMessage m(req, makeAddr());
    EXPECT_FALSE(m.getStatusInfo().has_value());
}

// ---- Pooling correctness: status info must be cleared when a message is reused ----
// SipMessage objects are pooled and reused via reset(); a recycled "200 OK" slot
// must not leak a stale status into the next message (e.g. a REGISTER request).
TEST(SipStatus, StatusInfoClearedOnReuse)
{
    SipMessage m(response("SIP/2.0 200 OK"), makeAddr());
    ASSERT_TRUE(m.getStatusInfo().has_value());
    EXPECT_EQ(m.getStatusInfo()->code, 200u);

    m.reset(
        "REGISTER sip:server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bK2\r\n"
        "From: <sip:100@server>\r\n"
        "To: <sip:100@server>\r\n"
        "Call-ID: id2\r\n"
        "CSeq: 2 REGISTER\r\n"
        "Content-Length: 0\r\n\r\n",
        makeAddr());
    EXPECT_FALSE(m.getStatusInfo().has_value());   // stale 200 must not linger
}
