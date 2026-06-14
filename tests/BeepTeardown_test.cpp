// BeepTeardown_test.cpp — issue #90: register-beep CANCEL/teardown must COMPLETE the
// INVITE client transaction (ACK the phone's 487 final response) instead of freeing the
// beep dialog the instant the CANCEL is queued.
//
// The #90 bug: tick() sent the CANCEL and immediately did `bd = BeepDialog{}`. When the
// phone's 487 Request Terminated arrived, findBeepByCallID() returned null, no ACK was
// sent (RFC 3261 §17.1.1.3 requires the UAC to ACK a non-2xx final response), and the
// phone retransmitted the 487 — the "487 storm" + stray 481 seen on hardware.
//
// This drives a real beep through RequestsHandler (harness pattern from Teardown_test):
// a NEW REGISTER triggers sendRegisterBeep(); we capture that INVITE, reflect a 487 back
// as the phone would, and assert the engine ACKs it (and only once — the slot then frees).

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
#include <cctype>
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

// Full header line (incl. name) whose name matches `prefix` case-insensitively, e.g.
// headerLine(raw, "Call-ID:") -> "Call-ID: abc@1.2.3.4". Empty string if not found.
std::string headerLine(const std::string& raw, const std::string& prefix) {
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eol = raw.find("\r\n", pos);
        const size_t end = (eol == std::string::npos) ? raw.size() : eol;
        if (end == pos) break;  // blank line: end of headers
        std::string line = raw.substr(pos, end - pos);
        if (line.size() >= prefix.size()) {
            std::string head = line.substr(0, prefix.size());
            std::string want = prefix;
            std::transform(head.begin(), head.end(), head.begin(),
                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            std::transform(want.begin(), want.end(), want.begin(),
                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            if (head == want) return line;
        }
        if (eol == std::string::npos) break;
        pos = eol + 2;
    }
    return "";
}

struct Cap {
    std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> out;
    void clear() { out.clear(); }

    // Count outbound messages to `ip` whose START LINE begins with `method`.
    int countMethodTo(const std::string& ip, const std::string& method) const {
        int n = 0;
        for (const auto& p : out) {
            if (p.first.sin_addr.s_addr != inet_addr(ip.c_str())) continue;
            const std::string s = p.second->toString();
            if (s.rfind(method, 0) == 0) ++n;
        }
        return n;
    }
    std::shared_ptr<SipMessage> firstMethodTo(const std::string& ip, const std::string& method) const {
        for (const auto& p : out) {
            if (p.first.sin_addr.s_addr != inet_addr(ip.c_str())) continue;
            const std::string s = p.second->toString();
            if (s.rfind(method, 0) == 0) return p.second;
        }
        return nullptr;
    }
};

// Reflect the phone's final response to the captured beep INVITE: copies the INVITE's
// Via/From/Call-ID verbatim (as a real UA would) and adds the phone's To-tag.
std::shared_ptr<SipMessage> makeFinalForBeep(const std::shared_ptr<SipMessage>& invite,
                                             const std::string& statusLine,
                                             const std::string& phoneIp) {
    const std::string raw = invite->toString();
    const std::string via    = headerLine(raw, "Via:");
    const std::string from   = headerLine(raw, "From:");
    const std::string to     = headerLine(raw, "To:");
    const std::string callid = headerLine(raw, "Call-ID:");
    std::string resp =
        statusLine + "\r\n" +
        via + "\r\n" +
        from + "\r\n" +
        to + ";tag=ph487\r\n" +
        callid + "\r\n" +
        "CSeq: 1 INVITE\r\n"
        "Content-Length: 0\r\n\r\n";
    return RequestsHandler::getMessageFromPool(resp, makeAddr(phoneIp));
}

struct BeepFixture {
    Cap cap;
    RequestsHandler handler;
    const std::string ext   = "113";
    const std::string phone = "192.168.4.70";

    BeepFixture()
        : handler("192.168.4.1", 5060,
                  [this](const sockaddr_in& dest, std::shared_ptr<SipMessage> msg) {
                      cap.out.emplace_back(dest, msg);
                  }) {}

    // The beep INVITE the engine sends on a brand-new registration.
    std::shared_ptr<SipMessage> registerAndGetBeepInvite() {
        handler.handle(makeRegister(ext, phone));
        return cap.firstMethodTo(phone, "INVITE");
    }
};

} // namespace

// The heart of #90: when the phone 487s our (cancelled) beep INVITE, the engine MUST ACK
// it. Pre-fix the slot was already freed, so no ACK went out and the phone retransmitted.
TEST(BeepTeardown, FinalResponseToBeepInviteIsAcked) {
    BeepFixture f;
    auto invite = f.registerAndGetBeepInvite();
    ASSERT_NE(invite, nullptr) << "a new REGISTER must emit a register-beep INVITE";

    f.cap.clear();
    f.handler.handle(makeFinalForBeep(invite, "SIP/2.0 487 Request Terminated", f.phone));

    auto ack = f.cap.firstMethodTo(f.phone, "ACK");
    ASSERT_NE(ack, nullptr)
        << "the 487 to our beep INVITE must be ACKed (RFC 3261 §17.1.1.3) — #90 regression";

    // ACK must stay in the INVITE transaction: same Call-ID as the beep INVITE.
    const std::string ackRaw = ack->toString();
    const std::string cid = headerLine(invite->toString(), "Call-ID:");
    ASSERT_FALSE(cid.empty());
    EXPECT_NE(ackRaw.find(cid), std::string::npos)
        << "ACK must carry the beep INVITE's Call-ID";
}

// After the 487 is ACKed the dialog is torn down; a retransmitted 487 finds no slot and
// must NOT produce a second ACK (proves the slot frees only AFTER teardown completes).
TEST(BeepTeardown, RetransmittedFinalDoesNotReAck) {
    BeepFixture f;
    auto invite = f.registerAndGetBeepInvite();
    ASSERT_NE(invite, nullptr);

    f.cap.clear();
    f.handler.handle(makeFinalForBeep(invite, "SIP/2.0 487 Request Terminated", f.phone));
    f.handler.handle(makeFinalForBeep(invite, "SIP/2.0 487 Request Terminated", f.phone));

    EXPECT_EQ(f.cap.countMethodTo(f.phone, "ACK"), 1)
        << "exactly one ACK — the slot must free after the first 487 is ACKed";
}
