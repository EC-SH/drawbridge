// Transaction_test.cpp — RFC 3261 §17 INVITE client transaction retransmit layer.
//
// Covers:
//   * getViaBranch() / getCSeqMethod() pure-string extraction
//   * Timer A retransmit: INVITE is re-sent when T1 elapses with no provisional
//   * 1xx stops retransmit: after a 180 Ringing, tick() must NOT re-send the INVITE
//   * Cancel stops retransmit: ghost-ring regression — a CANCEL before any response
//     must prevent the INVITE from being retransmitted by a later tick()

#include <gtest/gtest.h>
#include "RequestsHandler.hpp"
#include "SipMessage.hpp"
#include "SipMessageTypes.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

sockaddr_in makeAddr(const std::string& ip, uint16_t port = 5060) {
    sockaddr_in s{};
    s.sin_family   = AF_INET;
    s.sin_addr.s_addr = inet_addr(ip.c_str());
    s.sin_port     = htons(port);
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

std::shared_ptr<SipMessage> makeRegister(const std::string& ext, const std::string& ip) {
    std::string raw =
        "REGISTER sip:server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + ip + ":5060;branch=z9hG4bKr" + ext + "\r\n"
        "From: <sip:" + ext + "@server>;tag=rt" + ext + "\r\n"
        "To: <sip:" + ext + "@server>\r\n"
        "Call-ID: reg-" + ext + "\r\n"
        "CSeq: 1 REGISTER\r\n"
        "Contact: <sip:" + ext + "@" + ip + ":5060>;expires=3600\r\n"
        "Content-Length: 0\r\n\r\n";
    return RequestsHandler::getMessageFromPool(raw, makeAddr(ip));
}

std::shared_ptr<SipMessage> makeInvite(const std::string& from, const std::string& to,
                                       const std::string& srcIp, const std::string& callID,
                                       const std::string& branch = "") {
    std::string body = sdpBody(srcIp);
    std::string br   = branch.empty() ? ("z9hG4bK" + callID) : branch;
    std::string raw  =
        "INVITE sip:" + to + "@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + srcIp + ":5060;branch=" + br + "\r\n"
        "From: <sip:" + from + "@server>;tag=ft" + from + "\r\n"
        "To: <sip:" + to + "@server>\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 1 INVITE\r\n"
        "Contact: <sip:" + from + "@" + srcIp + ":5060>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    return RequestsHandler::getMessageFromPool(raw, makeAddr(srcIp));
}

// 180 Ringing sent by the callee back through the proxy path.
std::shared_ptr<SipMessage> makeRinging(const std::string& callID, const std::string& branch,
                                        const std::string& srcIp) {
    std::string raw =
        "SIP/2.0 180 Ringing\r\n"
        "Via: SIP/2.0/UDP " + srcIp + ":5060;branch=" + branch + "\r\n"
        "From: <sip:100@server>;tag=ft100\r\n"
        "To: <sip:101@server>;tag=tt101\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 1 INVITE\r\n"
        "Contact: <sip:101@" + srcIp + ":5060>\r\n"
        "Content-Length: 0\r\n\r\n";
    return RequestsHandler::getMessageFromPool(raw, makeAddr(srcIp));
}

// CANCEL sent by the caller.
std::shared_ptr<SipMessage> makeCancel(const std::string& from, const std::string& to,
                                       const std::string& srcIp, const std::string& callID,
                                       const std::string& branch) {
    std::string raw =
        "CANCEL sip:" + to + "@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + srcIp + ":5060;branch=" + branch + "\r\n"
        "From: <sip:" + from + "@server>;tag=ft" + from + "\r\n"
        "To: <sip:" + to + "@server>\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 1 CANCEL\r\n"
        "Content-Length: 0\r\n\r\n";
    return RequestsHandler::getMessageFromPool(raw, makeAddr(srcIp));
}

// Find the first captured INVITE for `callId` destined for `toIp`.
// getCallID() returns the full header line "Call-ID: <value>", so we search
// for the callId value anywhere in that string.
const std::shared_ptr<SipMessage>* findInviteByCallId(
    const std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>>& out,
    const std::string& toIp,
    const std::string& callId)
{
    uint32_t needle = inet_addr(toIp.c_str());
    for (const auto& [addr, msg] : out)
    {
        if (addr.sin_addr.s_addr == needle &&
            msg && msg->getType() == "INVITE" &&
            std::string(msg->getCallID()).find(callId) != std::string::npos)
        {
            return &msg;
        }
    }
    return nullptr;
}

// ── Pure unit tests for the new SipMessage helpers ───────────────────────────

TEST(SipMessageHelpers, GetViaBranchExtractsToken) {
    std::string raw =
        "INVITE sip:101@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 1.2.3.4:5060;rport;branch=z9hG4bKabc123\r\n"
        "From: <sip:100@server>;tag=x\r\n"
        "To: <sip:101@server>\r\n"
        "Call-ID: cid\r\n"
        "CSeq: 1 INVITE\r\n"
        "Content-Length: 0\r\n\r\n";
    auto msg = RequestsHandler::getMessageFromPool(raw, makeAddr("1.2.3.4"));
    EXPECT_EQ(msg->getViaBranch(), "z9hG4bKabc123");
}

TEST(SipMessageHelpers, GetViaBranchWithTrailingParams) {
    std::string raw =
        "SIP/2.0 180 Ringing\r\n"
        "Via: SIP/2.0/UDP 1.2.3.4:5060;branch=z9hG4bKxyz;received=5.6.7.8\r\n"
        "From: <sip:100@server>;tag=x\r\n"
        "To: <sip:101@server>;tag=y\r\n"
        "Call-ID: cid\r\n"
        "CSeq: 1 INVITE\r\n"
        "Content-Length: 0\r\n\r\n";
    auto msg = RequestsHandler::getMessageFromPool(raw, makeAddr("1.2.3.4"));
    EXPECT_EQ(msg->getViaBranch(), "z9hG4bKxyz");
}

TEST(SipMessageHelpers, GetViaBranchEmptyWhenAbsent) {
    std::string raw =
        "INVITE sip:101@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 1.2.3.4:5060;rport\r\n"
        "From: <sip:100@server>;tag=x\r\n"
        "To: <sip:101@server>\r\n"
        "Call-ID: cid\r\n"
        "CSeq: 1 INVITE\r\n"
        "Content-Length: 0\r\n\r\n";
    auto msg = RequestsHandler::getMessageFromPool(raw, makeAddr("1.2.3.4"));
    EXPECT_TRUE(msg->getViaBranch().empty());
}

TEST(SipMessageHelpers, GetCSeqMethodExtractsInvite) {
    std::string raw =
        "INVITE sip:101@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 1.2.3.4:5060;branch=z9hG4bKx\r\n"
        "From: <sip:100@server>;tag=x\r\n"
        "To: <sip:101@server>\r\n"
        "Call-ID: cid\r\n"
        "CSeq: 1 INVITE\r\n"
        "Content-Length: 0\r\n\r\n";
    auto msg = RequestsHandler::getMessageFromPool(raw, makeAddr("1.2.3.4"));
    EXPECT_EQ(msg->getCSeqMethod(), "INVITE");
}

TEST(SipMessageHelpers, GetCSeqMethodFromResponse) {
    std::string raw =
        "SIP/2.0 200 OK\r\n"
        "Via: SIP/2.0/UDP 1.2.3.4:5060;branch=z9hG4bKx\r\n"
        "From: <sip:100@server>;tag=x\r\n"
        "To: <sip:101@server>;tag=y\r\n"
        "Call-ID: cid\r\n"
        "CSeq: 1 REGISTER\r\n"
        "Content-Length: 0\r\n\r\n";
    auto msg = RequestsHandler::getMessageFromPool(raw, makeAddr("1.2.3.4"));
    EXPECT_EQ(msg->getCSeqMethod(), "REGISTER");
}

// ── Integration tests through RequestsHandler ────────────────────────────────

struct TxFixture {
    std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> out;
    RequestsHandler handler;
    const std::string callerIp = "192.168.4.50";
    const std::string calleeIp = "192.168.4.51";

    TxFixture()
        : handler("192.168.4.1", 5060,
                  [this](const sockaddr_in& dest, std::shared_ptr<SipMessage> msg) {
                      out.emplace_back(dest, std::move(msg));
                  }) {
        handler.handle(makeRegister("100", callerIp));
        handler.handle(makeRegister("101", calleeIp));
        out.clear();
    }
};

// Tick() gating: the 1 Hz gate means we must sleep > 1 s between ticks for the
// sweep to fire. Helper sleeps 1200 ms then calls tick().
static void sleepAndTick(RequestsHandler& h) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    h.tick();
}

TEST(Transaction, InviteForkRetransmitsAfterT1) {
    TxFixture f;
    const std::string callID = "tx-retransmit-1";
    const std::string branch = "z9hG4bKtx1";

    // 100 dials 101; engine forks an INVITE to 101 (.51)
    f.handler.handle(makeInvite("100", "101", f.callerIp, callID, branch));
    const auto* fork = findInviteByCallId(f.out, f.calleeIp, callID);
    ASSERT_NE(fork, nullptr) << "engine must fork an INVITE to callee";
    f.out.clear();

    // Sleep past T1=500 ms and the 1 Hz tick gate, then fire tick().
    // sweepTransactions() should retransmit the INVITE (still in Calling state).
    sleepAndTick(f.handler);

    const auto* retx = findInviteByCallId(f.out, f.calleeIp, callID);
    EXPECT_NE(retx, nullptr)
        << "sweepTransactions must retransmit INVITE to callee after T1 with no response";
}

TEST(Transaction, ProvisionalStopsRetransmit) {
    TxFixture f;
    const std::string callID = "tx-ringing-1";
    const std::string branch = "z9hG4bKring";

    f.handler.handle(makeInvite("100", "101", f.callerIp, callID, branch));
    f.out.clear();

    // Callee sends 180 Ringing → matchAndAdvanceTx advances tx to Proceeding.
    f.handler.handle(makeRinging(callID, branch, f.calleeIp));
    f.out.clear();

    sleepAndTick(f.handler);

    const auto* retx = findInviteByCallId(f.out, f.calleeIp, callID);
    EXPECT_EQ(retx, nullptr)
        << "180 Ringing must stop INVITE retransmit (Proceeding state)";
}

// Ghost-ring regression: if the INVITE was lost and the caller sends CANCEL
// before any provisional arrives, a later tick() must NOT retransmit the INVITE
// to the callee (that would ring the phone after the call is dead).
TEST(Transaction, CancelBeforeResponseStopsRetransmit) {
    TxFixture f;
    const std::string callID = "tx-cancel-1";
    const std::string branch = "z9hG4bKcan";

    f.handler.handle(makeInvite("100", "101", f.callerIp, callID, branch));
    f.out.clear();

    // Caller cancels (INVITE was "lost" — callee never got it).
    f.handler.handle(makeCancel("100", "101", f.callerIp, callID, branch));
    f.out.clear();

    sleepAndTick(f.handler);

    const auto* retx = findInviteByCallId(f.out, f.calleeIp, callID);
    EXPECT_EQ(retx, nullptr)
        << "CANCEL must free tx slot — tick() must not retransmit INVITE after cancel";
}

} // namespace
