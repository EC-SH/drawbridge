// Syslog_test.cpp — issue #129: RFC 5424 UDP syslog sink. Host has real sockets
// (unlike NVS), so these tests bind a real UDP listener on loopback and verify
// the actual datagram Syslog::send() puts on the wire, rather than mocking.

#include <gtest/gtest.h>
#include "Syslog.hpp"

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
#include <string>

namespace
{
	// Binds a UDP listener on 127.0.0.1:port and returns the fd, or -1 on failure.
	int bindUdpListener(uint16_t port)
	{
		int fd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
		if (fd < 0) return -1;
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
		{
#if defined(_WIN32) || defined(_WIN64)
			closesocket(fd);
#else
			close(fd);
#endif
			return -1;
		}
#if defined(_WIN32) || defined(_WIN64)
		DWORD tv = 500;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
		timeval tv{};
		tv.tv_sec = 0;
		tv.tv_usec = 500000;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
		return fd;
	}

	// Blocking (bounded by the listener's SO_RCVTIMEO) receive of one datagram.
	// Empty string on timeout/error.
	std::string recvOne(int fd)
	{
		char buf[1024] = {0};
#if defined(_WIN32) || defined(_WIN64)
		int n = recv(fd, buf, sizeof(buf), 0);
#else
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
#endif
		if (n <= 0) return "";
		return std::string(buf, static_cast<size_t>(n));
	}

	void closeFd(int fd)
	{
#if defined(_WIN32) || defined(_WIN64)
		closesocket(fd);
#else
		close(fd);
#endif
	}
}

TEST(Syslog, DisabledByDefaultSendIsNoOp)
{
	Syslog::configure("");   // explicit disable, regardless of test ordering
	EXPECT_FALSE(Syslog::isConfigured());
	// Must not crash/throw when unconfigured.
	Syslog::send(Syslog::Severity::Info, "pbx-test", "should be dropped silently");
}

TEST(Syslog, ConfigureRejectsNonNumericHost)
{
	EXPECT_FALSE(Syslog::configure("not-an-ip"));
	EXPECT_FALSE(Syslog::isConfigured());
}

TEST(Syslog, SendEmitsWellFormedRfc5424Datagram)
{
	int listener = bindUdpListener(15514);
	ASSERT_GE(listener, 0);

	ASSERT_TRUE(Syslog::configure("127.0.0.1", 15514));
	ASSERT_TRUE(Syslog::isConfigured());

	Syslog::send(Syslog::Severity::Info, "pbx-call", "caller=310 callee=210 duration=45 result=answered");

	std::string datagram = recvOne(listener);
	ASSERT_FALSE(datagram.empty()) << "expected a UDP datagram to arrive at the listener";

	// <PRI>1 - - drawbridge pbx-call - - caller=310 callee=210 duration=45 result=answered
	// PRI = facility(16)*8 + severity(6) = 134
	EXPECT_EQ(datagram.rfind("<134>1 - - drawbridge pbx-call - - ", 0), 0u)
		<< "got: " << datagram;
	EXPECT_NE(datagram.find("caller=310 callee=210 duration=45 result=answered"), std::string::npos);

	Syslog::configure("");   // disable again so later tests start clean
	closeFd(listener);
}

TEST(Syslog, EmptyHostDisablesSink)
{
	ASSERT_TRUE(Syslog::configure("127.0.0.1", 15515));
	ASSERT_TRUE(Syslog::isConfigured());
	ASSERT_TRUE(Syslog::configure(""));
	EXPECT_FALSE(Syslog::isConfigured());
}
