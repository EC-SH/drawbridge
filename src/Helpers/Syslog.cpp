#include "Syslog.hpp"

#include <mutex>
#include <cstdio>
#include <cstring>

#if defined(_WIN32) || defined(_WIN64)
	#include <WinSock2.h>
	#include <ws2tcpip.h>
#else
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <unistd.h>
#endif

#if defined(ESP_PLATFORM) || defined(ESP32)
	#include "nvs_flash.h"
	#include "nvs.h"
#endif

namespace
{
	constexpr auto NVS_PBX_NS = "pbxcfg";
	constexpr int  kFacilityLocal0 = 16;   // RFC 5424 facility 16 = local0

	struct SyslogState
	{
		std::mutex  mutex;
		bool        configured = false;
		int         sock = -1;
	};

	SyslogState& state()
	{
		static SyslogState s;
		return s;
	}

	// Caller must hold state().mutex.
	void closeSocketLocked(SyslogState& s)
	{
		if (s.sock >= 0)
		{
#if defined(_WIN32) || defined(_WIN64)
			closesocket(s.sock);
#else
			close(s.sock);
#endif
			s.sock = -1;
		}
		s.configured = false;
	}
}

namespace Syslog
{
	bool configure(const std::string& host, uint16_t port)
	{
		SyslogState& s = state();
		std::lock_guard<std::mutex> lock(s.mutex);

		closeSocketLocked(s);   // idempotent: also handles the "disable" (empty host) case

		if (host.empty())
		{
			return true;   // successfully disabled
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
		{
			return false;   // not a parseable numeric IPv4 address — stay disabled
		}

		int fd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
		if (fd < 0)
		{
			return false;
		}

		// connect() on a UDP socket just sets the default peer for send() — no
		// handshake, still connectionless and non-blocking.
		if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
		{
#if defined(_WIN32) || defined(_WIN64)
			closesocket(fd);
#else
			close(fd);
#endif
			return false;
		}

		s.sock = fd;
		s.configured = true;
		return true;
	}

	bool isConfigured()
	{
		SyslogState& s = state();
		std::lock_guard<std::mutex> lock(s.mutex);
		return s.configured;
	}

	void loadFromNvs()
	{
#if defined(ESP_PLATFORM) || defined(ESP32)
		nvs_handle_t h;
		if (nvs_open(NVS_PBX_NS, NVS_READONLY, &h) != ESP_OK)
		{
			return;
		}
		char hostBuf[64] = {0};
		size_t hostLen = sizeof(hostBuf);
		esp_err_t hostErr = nvs_get_str(h, "syslog_host", hostBuf, &hostLen);

		uint32_t port = 514;
		nvs_get_u32(h, "syslog_port", &port);   // absent key just leaves the default
		nvs_close(h);

		if (hostErr == ESP_OK && hostBuf[0] != '\0')
		{
			configure(hostBuf, static_cast<uint16_t>(port));
		}
#endif
		// Host builds: no NVS. Tests call configure() directly.
	}

	void send(Severity severity, const std::string& appName, const std::string& msg)
	{
		SyslogState& s = state();
		std::lock_guard<std::mutex> lock(s.mutex);
		if (!s.configured)
		{
			return;
		}

		// <PRI>1 TIMESTAMP HOSTNAME APP-NAME PROCID MSGID STRUCTURED-DATA MSG
		// TIMESTAMP/HOSTNAME/PROCID/MSGID/STRUCTURED-DATA all NILVALUE "-" (RFC 5424 §6.2-6.5)
		// — this device has no guaranteed wall clock without NTP.
		int pri = kFacilityLocal0 * 8 + static_cast<int>(severity);
		char buf[512];
		int n = std::snprintf(buf, sizeof(buf), "<%d>1 - - drawbridge %s - - %s",
		                       pri, appName.c_str(), msg.c_str());
		if (n <= 0)
		{
			return;
		}
		size_t len = (static_cast<size_t>(n) < sizeof(buf)) ? static_cast<size_t>(n) : sizeof(buf) - 1;

		// Fire-and-forget: a single-datagram UDP send never blocks waiting for the
		// peer, and we don't check/retry the result — a dropped syslog line is not
		// worth risking the calling thread (which may be under the registrar lock).
#if defined(_WIN32) || defined(_WIN64)
		::send(s.sock, buf, static_cast<int>(len), 0);
#else
		::send(s.sock, buf, len, 0);
#endif
	}
}
