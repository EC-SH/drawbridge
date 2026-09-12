#ifndef SIP_WIRE_UTIL_HPP
#define SIP_WIRE_UTIL_HPP

// Shared response-Via plumbing for RequestsHandler. Kept out of any pure
// string-work header deliberately — this one pulls in socket headers.

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include <lwip/sockets.h>
#elif defined(__linux__)
#include <arpa/inet.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#include <ws2tcpip.h>   // inet_ntop / INET_ADDRSTRLEN live here, not in WinSock2.h
#endif

#include <string>
#include <string_view>

namespace sipwire
{
	// The top Via of a response, stamped per RFC 3261 §18.2.1 and RFC 3581 §4:
	// `received` is the address the request ACTUALLY arrived from, and a bare
	// `rport` the sender asked for is answered with the source PORT.
	//
	// This used to append ";received=" + the SERVER's own IP, which is not a
	// cosmetic slip. A pjsip-based phone compares `received` against the address
	// in its own Contact, reads the mismatch as NAT, and rewrites its Contact to
	// what we told it — so it advertises <phone-port>@<PBX-ip>. Every subsequent
	// in-dialog request the far end routes through that Contact (re-INVITE for
	// hold/resume, REFER for an attended transfer, BYE) is then addressed to a
	// host:port pair where nothing is listening, and simply retransmits until it
	// times out. On loopback that shows up as a hold that never resumes; on a LAN
	// it is every pjsip phone becoming unreachable mid-call. Confirmed on hardware
	// via pocket-dial's tests/interop/ harness against real pjsip/baresip stacks
	// (2026-09-05); ported here as drawbridge #146.
	//
	// `via` is the request's top Via verbatim, INCLUDING its "Via:" header name
	// (that is what SipMessage::getVia() returns and setVia() expects back).
	inline std::string viaWithReceived(std::string_view via, const sockaddr_in& src)
	{
		char ipBuf[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &src.sin_addr, ipBuf, sizeof(ipBuf));

		std::string out(via);
		// A bare ";rport" (no value) is the sender asking to be told its source
		// port; give it the real one in place. ";rport=" already carrying a value
		// is left alone — it is not ours to rewrite.
		const size_t rp = out.find(";rport");
		if (rp != std::string::npos)
		{
			const size_t after = rp + 6;   // strlen(";rport")
			const char next = (after < out.size()) ? out[after] : '\0';
			if (next != '=')
			{
				out.insert(after, "=" + std::to_string(ntohs(src.sin_port)));
			}
		}
		out += ";received=";
		out += ipBuf;
		return out;
	}
}

#endif
