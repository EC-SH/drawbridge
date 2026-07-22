// Metrics_test.cpp — issue #128: GET /metrics returns Prometheus text-exposition
// format, unauthenticated (like /api/status — aggregate counts only, no secrets),
// with the new monotonic since-boot counters (sip_registrations_total /
// sip_calls_total) actually incrementing as REGISTER/INVITE traffic flows.

#include <gtest/gtest.h>
#include "HttpServer.hpp"
#include "RequestsHandler.hpp"
#include "SipMessage.hpp"

#if defined(_WIN32) || defined(_WIN64)
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include <chrono>
#include <thread>

namespace
{
	std::string httpGetRaw(int port, const std::string& path)
	{
#if defined(_WIN32) || defined(_WIN64)
		SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCKET) return "";
#else
		int s = socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0) return "";
#endif
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<uint16_t>(port));
		inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
		{
#if defined(_WIN32) || defined(_WIN64)
			closesocket(s);
#else
			close(s);
#endif
			return "";
		}
		std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
		send(s, req.c_str(), static_cast<int>(req.size()), 0);
		std::string resp;
		char buf[512];
		int n;
		while ((n = recv(s, buf, sizeof(buf), 0)) > 0)
		{
			resp.append(buf, static_cast<size_t>(n));
		}
#if defined(_WIN32) || defined(_WIN64)
		closesocket(s);
#else
		close(s);
#endif
		return resp;
	}

	int statusOf(const std::string& resp)
	{
		size_t sp1 = resp.find(' ');
		if (sp1 == std::string::npos) return -1;
		size_t sp2 = resp.find(' ', sp1 + 1);
		if (sp2 == std::string::npos) return -1;
		return std::atoi(resp.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
	}

	std::shared_ptr<SipMessage> makeRegisterFor(const std::string& from, const std::string& srcIp,
	                                             const std::string& callId)
	{
		sockaddr_in s{}; s.sin_family = AF_INET;
		s.sin_addr.s_addr = inet_addr(srcIp.c_str());
		s.sin_port = htons(5060);
		std::string raw =
			"REGISTER sip:server SIP/2.0\r\n"
			"Via: SIP/2.0/UDP " + srcIp + ":5060;branch=z9hG4bKr\r\n"
			"From: <sip:" + from + "@server>;tag=rt\r\n"
			"To: <sip:" + from + "@server>\r\n"
			"Call-ID: " + callId + "\r\n"
			"CSeq: 1 REGISTER\r\n"
			"Contact: <sip:" + from + "@" + srcIp + ":5060>;expires=3600\r\n"
			"Content-Length: 0\r\n\r\n";
		return RequestsHandler::getMessageFromPool(raw, s);
	}
}

TEST(Metrics, GetMetricsReturns200PrometheusText)
{
	HttpServer server("127.0.0.1", 18090, nullptr);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	std::string resp = httpGetRaw(18090, "/metrics");
	EXPECT_EQ(statusOf(resp), 200);
	EXPECT_NE(resp.find("Content-Type: text/plain"), std::string::npos);
	EXPECT_NE(resp.find("# TYPE uptime_seconds gauge"), std::string::npos);
	EXPECT_NE(resp.find("# TYPE sip_calls_total counter"), std::string::npos);
	EXPECT_NE(resp.find("sip_registrations_active "), std::string::npos);
}

TEST(Metrics, RegistrationsAndCallsTotalsIncrementWithTraffic)
{
	RequestsHandler handler("127.0.0.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	HttpServer server("127.0.0.1", 18091, nullptr);
	server.attachHandler(&handler);
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	std::string before = httpGetRaw(18091, "/metrics");
	handler.handle(makeRegisterFor("310", "127.0.0.201", "metrics-call-1"));

	std::string after = httpGetRaw(18091, "/metrics");
	EXPECT_NE(after.find("sip_registrations_total"), std::string::npos);
	// A brand-new binding must have bumped the total by at least 1 relative to a
	// board that had never registered "310" before.
	auto totalOf = [](const std::string& text) {
		size_t pos = text.find("\nsip_registrations_total ");
		if (pos == std::string::npos) return -1L;
		pos += 1; // skip leading \n
		size_t lineEnd = text.find('\n', pos);
		std::string line = text.substr(pos, lineEnd - pos);
		size_t sp = line.find(' ');
		return std::atol(line.substr(sp + 1).c_str());
	};
	EXPECT_GT(totalOf(after), totalOf(before));
}
