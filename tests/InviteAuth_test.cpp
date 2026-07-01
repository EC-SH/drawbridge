// InviteAuth_test.cpp — issue #125: SIP digest auth for INVITE.
//
// In Secure registrar mode an INITIAL INVITE must carry a valid
// Proxy-Authorization for the From-extension; otherwise a LAN host could place
// calls impersonating any extension despite REGISTER auth (THREAT_MODEL
// S-3/D-2). The server challenges with 407 Proxy Authentication Required and
// verifies the retried INVITE against the per-extension HA1 in SipSecretStore.
// Mid-dialog re-INVITEs (hold/resume/transfer) are exempt — already
// authenticated at call setup — and the ACK for the 407 is absorbed silently
// (RFC 3261 §22, no session was allocated for a challenged INVITE).
//
// These are the first end-to-end digest tests through RequestsHandler. The
// engine keeps process-wide static state (client/secret stores), so each test
// uses unique extensions and resets the registrar mode in a fixture teardown.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "RequestsHandler.hpp"
#include "SipMessage.hpp"
#include "SipDigest.hpp"
#include "SipSecretStore.hpp"

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

// Initial or authenticated INVITE. `proxyAuth`, when non-empty, is inserted as
// a Proxy-Authorization header (the digest-answer retry). `toTag` non-empty
// makes it a mid-dialog re-INVITE.
std::shared_ptr<SipMessage> makeInvite(const std::string& from, const std::string& to,
                                       const std::string& srcIp, const std::string& callID,
                                       int cseq = 1, const std::string& proxyAuth = "",
                                       const std::string& toTag = "") {
    std::string body = sdpBody(srcIp);
    std::string to_ = "To: <sip:" + to + "@server>";
    if (!toTag.empty()) to_ += ";tag=" + toTag;
    std::string raw =
        "INVITE sip:" + to + "@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bK" + callID + std::to_string(cseq) + "\r\n"
        "From: <sip:" + from + "@server>;tag=ft" + from + "\r\n"
        + to_ + "\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: " + std::to_string(cseq) + " INVITE\r\n"
        "Contact: <sip:" + from + "@" + srcIp + ":5060>\r\n";
    if (!proxyAuth.empty()) raw += "Proxy-Authorization: " + proxyAuth + "\r\n";
    raw +=
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    return RequestsHandler::getMessageFromPool(raw, makeAddr(srcIp));
}

std::shared_ptr<SipMessage> makeAck(const std::string& from, const std::string& to,
                                    const std::string& srcIp, const std::string& callID,
                                    const std::string& toTag) {
    std::string raw =
        "ACK sip:" + to + "@server SIP/2.0\r\n"
        "Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bKack" + callID + "\r\n"
        "From: <sip:" + from + "@server>;tag=ft" + from + "\r\n"
        "To: <sip:" + to + "@server>;tag=" + toTag + "\r\n"
        "Call-ID: " + callID + "\r\n"
        "CSeq: 1 ACK\r\n"
        "Content-Length: 0\r\n\r\n";
    return RequestsHandler::getMessageFromPool(raw, makeAddr(srcIp));
}

// Capture harness: records every message the engine emits.
struct Captured {
    std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> out;
    std::shared_ptr<SipMessage> byStatus(int code) const {
        for (const auto& e : out) {
            auto st = e.second->getStatusInfo();
            if (st.has_value() && st->code == code) return e.second;
        }
        return nullptr;
    }
    bool anyStatus(int code) const { return byStatus(code) != nullptr; }
};

// Build the Proxy-Authorization value a compliant phone would send in reply to
// the server's 407, extracting the nonce from the challenge.
std::string buildProxyAuth(const std::string& challenge407, const std::string& ext,
                           const std::string& secret, const std::string& toNumber) {
    std::smatch m;
    std::string raw = challenge407;
    std::regex nonceRe("nonce=\"([^\"]+)\"");
    EXPECT_TRUE(std::regex_search(raw, m, nonceRe)) << "407 must carry a nonce";
    std::string nonce = m[1];

    const std::string realm = SipSecretStore::kRealm;
    const std::string uri = "sip:" + toNumber + "@server";
    const std::string nc = "00000001";
    const std::string cnonce = "cafebabe";
    const std::string qop = "auth";

    std::string ha1 = SipDigest::computeHa1(ext, realm, secret);
    std::string resp = SipDigest::computeResponse(ha1, "INVITE", uri, nonce, nc, cnonce, qop);

    return "Digest username=\"" + ext + "\", realm=\"" + realm + "\", nonce=\"" + nonce +
           "\", uri=\"" + uri + "\", response=\"" + resp + "\", qop=" + qop +
           ", nc=" + nc + ", cnonce=\"" + cnonce + "\", algorithm=MD5";
}

// Fixture: two registered extensions, a secret for the caller, Secure mode.
struct InviteAuthTest : public ::testing::Test {
    Captured cap;
    std::unique_ptr<RequestsHandler> handler;
    std::string caller;
    std::string callee;
    std::string secret = "s3cr3t-pass";
    std::string callerIp = "192.168.9.10";
    std::string calleeIp = "192.168.9.11";

    void configure(const std::string& c, const std::string& ce) {
        caller = c; callee = ce;
        handler = std::make_unique<RequestsHandler>("192.168.9.1", 5060,
            [this](const sockaddr_in& dest, std::shared_ptr<SipMessage> msg) {
                cap.out.emplace_back(dest, std::move(msg));
            });
        // Register both in Open mode (default), then provision + go Secure.
        handler->handle(makeRegister(caller, callerIp));
        handler->handle(makeRegister(callee, calleeIp));
        ASSERT_TRUE(SipSecretStore::setSecret(caller, secret));
        handler->setRegistrarMode(RequestsHandler::RegistrarMode::Secure);
        cap.out.clear();
    }

    void TearDown() override {
        if (handler) handler->setRegistrarMode(RequestsHandler::RegistrarMode::Open);
        SipSecretStore::clearSecret(caller);
    }
};

} // namespace

// An initial INVITE with no credentials is challenged with 407, and no 100/180
// is leaked (the challenge is the only response) and no session is allocated.
TEST_F(InviteAuthTest, UnauthenticatedInviteIsChallenged407) {
    configure("5100", "5101");
    handler->handle(makeInvite(caller, callee, callerIp, "ia-1"));

    auto ch = cap.byStatus(407);
    ASSERT_TRUE(ch != nullptr) << "initial INVITE must be challenged 407 in Secure mode";
    const std::string raw = ch->toString();
    EXPECT_NE(raw.find("Proxy-Authenticate"), std::string::npos)
        << "407 must carry a Proxy-Authenticate header (not WWW-Authenticate)";
    EXPECT_NE(raw.find("Digest"), std::string::npos);
    EXPECT_NE(raw.find("realm=\"pocketdial\""), std::string::npos);

    // No INVITE was relayed to the callee: the call did not proceed.
    bool relayed = false;
    for (auto& e : cap.out)
        if (e.second->getType() == "INVITE") relayed = true;
    EXPECT_FALSE(relayed) << "a challenged INVITE must not be relayed to the callee";
}

// The retried INVITE carrying a valid Proxy-Authorization proceeds: it is
// relayed to the callee (no 407, no 403).
TEST_F(InviteAuthTest, AuthenticatedInviteProceeds) {
    configure("5110", "5111");
    handler->handle(makeInvite(caller, callee, callerIp, "ia-2"));
    auto ch = cap.byStatus(407);
    ASSERT_TRUE(ch != nullptr);
    std::string pa = buildProxyAuth(ch->toString(), caller, secret, callee);
    cap.out.clear();

    handler->handle(makeInvite(caller, callee, callerIp, "ia-2", 2, pa));

    EXPECT_FALSE(cap.anyStatus(407)) << "a valid Proxy-Authorization must not be re-challenged";
    EXPECT_FALSE(cap.anyStatus(403)) << "a valid Proxy-Authorization must not be rejected";
    bool relayed = false;
    for (auto& e : cap.out)
        if (e.second->getType() == "INVITE" &&
            std::string(e.second->getCallID()).find("ia-2") != std::string::npos)
            relayed = true;
    EXPECT_TRUE(relayed) << "the authenticated INVITE must be relayed to the callee";
}

// A wrong password (bad digest response) is rejected 403, not relayed.
TEST_F(InviteAuthTest, WrongCredentialsRejected403) {
    configure("5120", "5121");
    handler->handle(makeInvite(caller, callee, callerIp, "ia-3"));
    auto ch = cap.byStatus(407);
    ASSERT_TRUE(ch != nullptr);
    // Build the answer with the WRONG secret.
    std::string pa = buildProxyAuth(ch->toString(), caller, "wrong-pass", callee);
    cap.out.clear();

    handler->handle(makeInvite(caller, callee, callerIp, "ia-3", 2, pa));
    EXPECT_TRUE(cap.anyStatus(403)) << "bad credentials must be rejected 403";
    bool relayed = false;
    for (auto& e : cap.out)
        if (e.second->getType() == "INVITE") relayed = true;
    EXPECT_FALSE(relayed);
}

// The ACK for the 407 hits no session and is absorbed silently (RFC 3261 §22).
TEST_F(InviteAuthTest, AckToChallengeIsAbsorbed) {
    configure("5130", "5131");
    handler->handle(makeInvite(caller, callee, callerIp, "ia-4"));
    ASSERT_TRUE(cap.byStatus(407) != nullptr);
    cap.out.clear();

    handler->handle(makeAck(caller, callee, callerIp, "ia-4", "st407"));
    EXPECT_TRUE(cap.out.empty()) << "the ACK for a 407 must produce no response";
}

// A star-code INVITE from a registered extension is trusted (no 407) — the
// caller is already proven registered, and star codes are the admin surface.
TEST_F(InviteAuthTest, StarCodeInviteExemptFromAuth) {
    configure("5140", "5141");
    // Destination *60 — registered==authenticated for star codes.
    handler->handle(makeInvite(caller, "*60", callerIp, "ia-5"));
    EXPECT_FALSE(cap.anyStatus(407)) << "star-code INVITEs from registered extensions are trusted";
}

// Open mode does not challenge INVITEs (regression guard: the gate is
// Secure-only).
TEST_F(InviteAuthTest, OpenModeDoesNotChallenge) {
    configure("5150", "5151");
    handler->setRegistrarMode(RequestsHandler::RegistrarMode::Open);
    cap.out.clear();
    handler->handle(makeInvite(caller, callee, callerIp, "ia-6"));
    EXPECT_FALSE(cap.anyStatus(407)) << "Open mode must not challenge INVITEs";
}
