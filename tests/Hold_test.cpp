// Hold_test.cpp — call hold/resume via mid-dialog re-INVITE (RFC 3261 §12.2 /
// RFC 3264 direction attributes).
//
// End-to-end through RequestsHandler (harness pattern from Rtp_test.cpp):
// register 100 + 101, establish a call, then drive a hold re-INVITE
// (a=sendonly) and a resume re-INVITE (a=sendrecv) and assert:
//   * the re-INVITE is relayed to the peer leg UNTOUCHED — body intact, with a
//     Content-Length that matches the body byte count (no clearBody/enforceG711),
//   * the dashboard snapshot (tick() + getActiveSessions()) reports "Held" and
//     flips back to "Connected" on resume,
//   * the re-INVITE 200 OK answer is relayed untouched to the opposite leg.
// Plus pure unit coverage for SipMessage::getSdpDirection().

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

std::string sdpBody(const std::string& ip, const std::string& direction) {
    std::string b =
        "v=0\r\n"
        "o=- 0 0 IN IP4 " + ip + "\r\n"
        "s=-\r\n"
        "c=IN IP4 " + ip + "\r\n"
        "t=0 0\r\n"
        "m=audio 40000 RTP/AVP 0 8 101\r\n"
        "a=rtpmap:0 PCMU/8000\r\n";
    if (!direction.empty()) b += "a=" + direction + "\r\n";
    return b;
}

std::shared_ptr<SipMessage> makeRegister(const std::string& from,
                                         const std::string& srcIp) {
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

// INVITE from `from` to `to`. An empty toTag makes the initial dialog-forming
// INVITE; a non-empty toTag makes a mid-dialog re-INVITE.
std::shared_ptr<SipMessage> makeInvite(const std::string& from,
                                       const std::string& to,
                                       const std::string& srcIp,
                                       const std::string& callID,
                                       int cseq,
                                       const std::string& direction,
                                       const std::string& fromTag,
                                       const std::string& toTag) {
    std::string body = sdpBody(srcIp, direction);
    std::string to_ = "To: <sip:" + to + "@server>";
    if (!toTag.empty()) to_ += ";tag=" + toTag;
    std::string head =
        "INVITE sip:" + to + "@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bK" + std::to_string(cseq) + "\r\n"
        "From: <sip:" + from + "@server>;tag=" + fromTag + "\r\n"
        + to_ + "\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: " + std::to_string(cseq) + " INVITE\r\n"
        "Contact: <sip:" + from + "@" + srcIp + ":5060>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    return RequestsHandler::getMessageFromPool(head + body, makeAddr(srcIp));
}

// 200 OK answer sent by `answerer` (the To party) from `srcIp`.
std::shared_ptr<SipMessage> makeOk(const std::string& from,
                                   const std::string& to,
                                   const std::string& srcIp,
                                   const std::string& callID,
                                   int cseq,
                                   const std::string& direction,
                                   const std::string& fromTag,
                                   const std::string& toTag) {
    std::string body = sdpBody(srcIp, direction);
    std::string raw =
        "SIP/2.0 200 OK\r\n"
        "Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bK" + std::to_string(cseq) + "\r\n"
        "From: <sip:" + from + "@server>;tag=" + fromTag + "\r\n"
        "To: <sip:" + to + "@server>;tag=" + toTag + "\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: " + std::to_string(cseq) + " INVITE\r\n"
        "Contact: <sip:" + to + "@" + srcIp + ":5060>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    return RequestsHandler::getMessageFromPool(raw + body, makeAddr(srcIp));
}

// A dialog-event SUBSCRIBE from `watcher` (at `srcIp`) watching extension `to`.
// expires=0 unsubscribes. Mirrors the headers RequestsHandler::onSubscribe parses.
std::shared_ptr<SipMessage> makeSubscribe(const std::string& watcher,
                                          const std::string& to,
                                          const std::string& srcIp,
                                          const std::string& callID,
                                          int cseq,
                                          int expires) {
    std::string raw =
        "SUBSCRIBE sip:" + to + "@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bKs" + std::to_string(cseq) + "\r\n"
        "From: <sip:" + watcher + "@server>;tag=fs" + watcher + "\r\n"
        "To: <sip:" + to + "@server>\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: " + std::to_string(cseq) + " SUBSCRIBE\r\n"
        "Contact: <sip:" + watcher + "@" + srcIp + ":5060>\r\n"
        "Event: dialog\r\n"
        "Expires: " + std::to_string(expires) + "\r\n"
        "Content-Length: 0\r\n\r\n";
    return RequestsHandler::getMessageFromPool(raw, makeAddr(srcIp));
}

struct Captured {
    std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> out;
    void clear() { out.clear(); }
};

// Test fixture: handler with 100 @ .50 and 101 @ .51 registered and a call
// 100 -> 101 fully established (INVITE relayed, 200 OK answered).
struct HoldFixture {
    Captured cap;
    RequestsHandler handler;
    const std::string callerIp = "192.168.4.50";
    const std::string calleeIp = "192.168.4.51";
    const std::string callID = "hold-call-1";

    HoldFixture()
        : handler("192.168.4.1", 5060,
                  [this](const sockaddr_in& dest, std::shared_ptr<SipMessage> msg) {
                      cap.out.emplace_back(dest, msg);
                  }) {
        handler.handle(makeRegister("100", callerIp));
        handler.handle(makeRegister("101", calleeIp));
        cap.clear();

        // 100 dials 101; 101 answers with sendrecv SDP.
        handler.handle(makeInvite("100", "101", callerIp, callID, 1,
                                  "sendrecv", "ft100", ""));
        handler.handle(makeOk("100", "101", calleeIp, callID, 1,
                              "sendrecv", "ft100", "tt101"));
        cap.clear();
    }

    std::string sessionState() {
        handler.tick();
        for (const auto& s : handler.getActiveSessions()) {
            return std::get<2>(s);
        }
        return "";
    }

    // Body of the most recent captured NOTIFY (a SIP request with no status code),
    // or "" if none. Used to read the dialog-info+xml BLF state the watcher sees.
    std::string lastNotifyBody() const {
        std::string body;
        for (const auto& ev : cap.out) {
            const std::string wire = ev.second->toString();
            if (wire.compare(0, 7, "NOTIFY ") != 0) continue;
            size_t sep = wire.find("\r\n\r\n");
            body = (sep == std::string::npos) ? "" : wire.substr(sep + 4);
        }
        return body;
    }
};

uint32_t ipOf(const sockaddr_in& a) { return a.sin_addr.s_addr; }

} // namespace

// ── getSdpDirection() unit coverage ──────────────────────────────────────────

TEST(SdpDirection, ParsesAllFourAttributes) {
    sockaddr_in s = makeAddr("127.0.0.1");
    auto mk = [&](const std::string& dir) {
        std::string body = sdpBody("127.0.0.1", dir);
        std::string raw =
            "INVITE sip:101@server SIP/2.0\r\n"
            "Content-Type: application/sdp\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        return SipMessage(raw, s).getSdpDirection();
    };
    EXPECT_EQ(mk("sendrecv"), SipMessage::SdpDirection::SendRecv);
    EXPECT_EQ(mk("sendonly"), SipMessage::SdpDirection::SendOnly);
    EXPECT_EQ(mk("recvonly"), SipMessage::SdpDirection::RecvOnly);
    EXPECT_EQ(mk("inactive"), SipMessage::SdpDirection::Inactive);
    EXPECT_EQ(mk(""), SipMessage::SdpDirection::None);   // absent attribute
}

TEST(SdpDirection, NoBodyIsNone) {
    sockaddr_in s = makeAddr("127.0.0.1");
    SipMessage m("BYE sip:101@server SIP/2.0\r\nContent-Length: 0\r\n\r\n", s);
    EXPECT_EQ(m.getSdpDirection(), SipMessage::SdpDirection::None);
}

TEST(SdpDirection, AttributeMustBeLineAnchored) {
    sockaddr_in s = makeAddr("127.0.0.1");
    // "sendonly" buried inside another line must NOT match.
    std::string body =
        "v=0\r\n"
        "s=this mentions a=sendonly mid-line\r\n"
        "m=audio 40000 RTP/AVP 0\r\n";
    std::string raw =
        "INVITE sip:101@server SIP/2.0\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    EXPECT_EQ(SipMessage(raw, s).getSdpDirection(), SipMessage::SdpDirection::None);
}

// ── Hold re-INVITE: relayed untouched, session goes Held ────────────────────

TEST(Hold, SendonlyReinviteRelayedIntactAndSessionHeld) {
    HoldFixture f;
    ASSERT_EQ(f.sessionState(), "Connected");

    // 100 puts the call on hold: mid-dialog re-INVITE (To carries the tag).
    auto reinvite = makeInvite("100", "101", f.callerIp, f.callID, 2,
                               "sendonly", "ft100", "tt101");
    const std::string wire = reinvite->toString();
    f.cap.clear();
    f.handler.handle(reinvite);

    // Exactly one relay, addressed to the callee leg (.51), byte-identical.
    ASSERT_EQ(f.cap.out.size(), 1u);
    EXPECT_EQ(ipOf(f.cap.out[0].first), inet_addr(f.calleeIp.c_str()));
    const std::string relayed = f.cap.out[0].second->toString();
    EXPECT_EQ(relayed, wire) << "re-INVITE must be relayed UNTOUCHED";

    // Body intact: hold attribute present, Content-Length matches body bytes.
    size_t sep = relayed.find("\r\n\r\n");
    ASSERT_NE(sep, std::string::npos);
    std::string body = relayed.substr(sep + 4);
    EXPECT_NE(body.find("a=sendonly\r\n"), std::string::npos);
    EXPECT_EQ(std::string(f.cap.out[0].second->getContentLength()),
              "Content-Length: " + std::to_string(body.size()));

    // Dashboard reports Held.
    EXPECT_EQ(f.sessionState(), "Held");

    // The callee's 200 OK answer (a=recvonly) relays untouched back to 100.
    auto answer = makeOk("100", "101", f.calleeIp, f.callID, 2,
                         "recvonly", "ft100", "tt101");
    const std::string answerWire = answer->toString();
    f.cap.clear();
    f.handler.handle(answer);
    ASSERT_EQ(f.cap.out.size(), 1u);
    EXPECT_EQ(ipOf(f.cap.out[0].first), inet_addr(f.callerIp.c_str()));
    EXPECT_EQ(f.cap.out[0].second->toString(), answerWire);

    // Still Held after the answer (the answer must not reset state).
    EXPECT_EQ(f.sessionState(), "Held");
}

TEST(Hold, SendrecvReinviteResumesToConnected) {
    HoldFixture f;

    // Hold...
    f.handler.handle(makeInvite("100", "101", f.callerIp, f.callID, 2,
                                "sendonly", "ft100", "tt101"));
    EXPECT_EQ(f.sessionState(), "Held");

    // ...resume with sendrecv.
    f.cap.clear();
    f.handler.handle(makeInvite("100", "101", f.callerIp, f.callID, 3,
                                "sendrecv", "ft100", "tt101"));
    ASSERT_EQ(f.cap.out.size(), 1u);
    EXPECT_EQ(ipOf(f.cap.out[0].first), inet_addr(f.calleeIp.c_str()));
    EXPECT_EQ(f.sessionState(), "Connected");

    // A re-INVITE with NO direction attribute also means active (RFC 3264).
    f.handler.handle(makeInvite("100", "101", f.callerIp, f.callID, 4,
                                "sendonly", "ft100", "tt101"));
    EXPECT_EQ(f.sessionState(), "Held");
    f.handler.handle(makeInvite("100", "101", f.callerIp, f.callID, 5,
                                "", "ft100", "tt101"));
    EXPECT_EQ(f.sessionState(), "Connected");
}

TEST(Hold, CalleeCanAlsoHold) {
    HoldFixture f;
    // 101 (the dest leg) holds: relay must go to the CALLER (.50).
    f.cap.clear();
    f.handler.handle(makeInvite("101", "100", f.calleeIp, f.callID, 2,
                                "sendonly", "tt101", "ft100"));
    ASSERT_EQ(f.cap.out.size(), 1u);
    EXPECT_EQ(ipOf(f.cap.out[0].first), inet_addr(f.callerIp.c_str()));
    EXPECT_EQ(f.sessionState(), "Held");
}

TEST(Hold, RetransmittedInitialInviteStillSilentlyDropped) {
    HoldFixture f;
    // A retransmission of the ORIGINAL INVITE (no To tag) for the Connected
    // session keeps the historical silent-drop behavior — no relay, no response.
    f.cap.clear();
    f.handler.handle(makeInvite("100", "101", f.callerIp, f.callID, 1,
                                "sendrecv", "ft100", ""));
    EXPECT_TRUE(f.cap.out.empty());
    EXPECT_EQ(f.sessionState(), "Connected");
}

// ── BLF / presence: a watched call on hold stays busy (#53) ──────────────────
//
// A 3rd extension (102 @ .52) watches 101 via a dialog-event SUBSCRIBE. While
// 100<->101 is Connected the watcher's lamp is lit (NOTIFY: confirmed). When the
// call is put on hold, computeDialogState() must STILL report confirmed — the
// line is busy-held, not idle.
//
// Regression mechanics: refreshSubscriptions() only re-NOTIFYs on a state-token
// *change*. With the fix, Connected and Held both map to "confirmed" so the token
// is unchanged and the lamp simply stays lit (no churn NOTIFY). The pre-fix bug
// mapped Held to idle, so the token flipped confirmed->idle and the watcher got
// an idle NOTIFY (empty dialog-info body, lamp dark). We therefore assert on the
// LAST NOTIFY the watcher ever received across the whole sequence: it must be
// confirmed. On the buggy code an idle NOTIFY overwrites it and this fails.

TEST(Blf, WatchedCallOnHoldStaysConfirmed) {
    HoldFixture f;
    const std::string watcherIp = "192.168.4.52";

    // 102 subscribes to 101. The immediate NOTIFY reports the established call.
    f.cap.clear();
    f.handler.handle(makeSubscribe("102", "101", watcherIp, "blf-sub-1", 1, 3600));
    {
        const std::string body = f.lastNotifyBody();
        ASSERT_FALSE(body.empty()) << "SUBSCRIBE must trigger an immediate NOTIFY";
        EXPECT_NE(body.find("<state>confirmed</state>"), std::string::npos)
            << "watched 101 is in an established call: lamp must be lit";
    }

    // 100 puts the call on hold (sendonly re-INVITE). Do NOT clear cap: we want the
    // most recent NOTIFY the watcher saw across SUBSCRIBE + hold.
    f.handler.handle(makeInvite("100", "101", f.callerIp, f.callID, 2,
                                "sendonly", "ft100", "tt101"));
    ASSERT_EQ(f.sessionState(), "Held");
    f.handler.tick();   // refreshSubscriptions(): NOTIFY only if the token changed

    const std::string body = f.lastNotifyBody();
    ASSERT_FALSE(body.empty()) << "watcher must have at least the SUBSCRIBE NOTIFY";
    EXPECT_NE(body.find("<state>confirmed</state>"), std::string::npos)
        << "held line is still busy: BLF lamp must stay lit (#53)";
    // An idle NOTIFY omits the <dialog id=...> element entirely (only the wrapping
    // <dialog-info> remains) — that empty body is the dark lamp. Match "<dialog id"
    // so the check doesn't collide with the "<dialog-info" wrapper tag.
    EXPECT_NE(body.find("<dialog id"), std::string::npos)
        << "an idle body (no <dialog id=...>) would dark the lamp — the #53 bug";
    EXPECT_EQ(body.find("<dialog id"), body.rfind("<dialog id"))
        << "exactly one dialog element";
}

TEST(Blf, WatchedCallStaysConfirmedAcrossHoldAndResume) {
    HoldFixture f;
    const std::string watcherIp = "192.168.4.52";
    f.handler.handle(makeSubscribe("102", "101", watcherIp, "blf-sub-2", 1, 3600));

    auto watchedStateAfterReinvite = [&](int cseq, const std::string& dir) {
        f.handler.handle(makeInvite("100", "101", f.callerIp, f.callID, cseq,
                                    dir, "ft100", "tt101"));
        f.cap.clear();
        f.handler.tick();
        const std::string body = f.lastNotifyBody();
        // tick() only NOTIFYs on a *changed* token; if unchanged, the lamp is
        // whatever the previous NOTIFY set — confirmed throughout this sequence.
        if (body.empty()) return std::string("unchanged");
        return body.find("<state>confirmed</state>") != std::string::npos
                   ? std::string("confirmed")
                   : std::string("idle");
    };

    // hold -> resume -> hold: never idle, the lamp never blinks off.
    EXPECT_NE(watchedStateAfterReinvite(2, "sendonly"), std::string("idle"));
    EXPECT_EQ(f.sessionState(), "Held");
    EXPECT_NE(watchedStateAfterReinvite(3, "sendrecv"), std::string("idle"));
    EXPECT_EQ(f.sessionState(), "Connected");
    EXPECT_NE(watchedStateAfterReinvite(4, "inactive"), std::string("idle"));
    EXPECT_EQ(f.sessionState(), "Held");
}
