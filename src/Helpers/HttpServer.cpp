// HttpServer.cpp: Issues #23 and #28 resolved.
#include "HttpServer.hpp"
#include "RequestsHandler.hpp"
#include "CallDetailRecord.hpp"
#include "AdminAuth.hpp"
#include "OtaUpdater.hpp"
#include "index_html.h"
#include "IPHelper.hpp"
#include "UrlEncode.hpp"
#include <cstring>
#include <sstream>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <vector>

#if defined(POCKETDIAL_HAS_WIFI)
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#endif

#if defined(ESP_PLATFORM)
// OTA reboot path needs esp_restart() + a deferred-restart FreeRTOS task. These
// are available on EVERY ESP transport (WiFi, Ethernet, display), not just
// POCKETDIAL_HAS_WIFI, so guard them on the platform rather than the transport.
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"   // heap_caps_get_*_size for PSRAM telemetry (#82)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

// Forward declarations for the file-local form/URL helpers (defined lower down).
// sendApiDnd() uses getFormParam() but is defined earlier in this TU.
static std::string getFormParam(const std::string& body, const std::string& key);
// base64Decode() is used by the OTA interception in handleClient() (near the top of
// this TU) but defined lower down; forward-declare it (issue #47).
static std::string base64Decode(const std::string& in);

// Captive-portal decay hold. The display app's decay watchdog reads this; the web
// "/api/configuring" confirm sets it to pause the auto-switch to Standalone while a user is
// actively configuring. Defined here so it links in every transport (the display references
// it via extern; other transports simply never read it).
volatile bool g_decayHold = false;

HttpServer::HttpServer(const std::string& ip, int port, RequestsHandler* handler)
	: _ip(ip), _port(port), _listenSock(-1), _handler(handler), _running(false)
{
	_startTime = currentTimeMs();

#if defined _WIN32 || defined _WIN64
	// WSAStartup may already be called by UdpServer, but calling it again is safe
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

	_listenSock = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
	if (_listenSock < 0)
	{
		throw std::runtime_error("HttpServer: TCP socket creation failed");
	}

	// Allow address reuse
	int opt = 1;
#if defined _WIN32 || defined _WIN64
	setsockopt(_listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
	setsockopt(_listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	// Bind to all interfaces (INADDR_ANY) rather than the one configured IP.
	// Binding a listening socket to a specific, dynamically-assigned address is
	// fragile on lwip: in Wi-Fi STATION mode the DHCP-assigned IP is bound here,
	// and connections to it were accepted by lwip into the backlog but never
	// serviced (dashboard unreachable on a LAN, while the SoftAP's static
	// 192.168.4.1 worked). INADDR_ANY serves on every interface/IP and is robust
	// across AP/STA mode switches and IP/lease changes. _ip is still used for
	// display/logging and the captive-portal same-origin check.
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(static_cast<uint16_t>(_port));

	if (bind(_listenSock, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) < 0)
	{
		closeSocket(_listenSock);
		throw std::runtime_error("HttpServer: bind failed on port " + std::to_string(_port));
	}

	if (listen(_listenSock, 8) < 0)
	{
		closeSocket(_listenSock);
		throw std::runtime_error("HttpServer: listen failed");
	}
}

HttpServer::~HttpServer()
{
	_running = false;
	// Close the listen socket to unblock accept()
	if (_listenSock >= 0)
	{
		shutdown(_listenSock, 2);
		closeSocket(_listenSock);
	}
	if (_acceptThread.joinable())
	{
		_acceptThread.join();
	}
}

void HttpServer::start()
{
	_running = true;
	_acceptThread = std::thread(&HttpServer::acceptLoop, this);
}

void HttpServer::acceptLoop()
{
	while (_running)
	{
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(static_cast<unsigned int>(_listenSock), &readfds);

		timeval tv{};
		tv.tv_sec = 0;
		tv.tv_usec = 250000; // 250ms timeout

		int activity = select(_listenSock + 1, &readfds, nullptr, nullptr, &tv);
		if (activity < 0)
		{
			if (!_running) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}
		if (activity == 0)
		{
			continue; // Timeout, loop and check _running
		}

		sockaddr_in clientAddr{};
#if defined _WIN32 || defined _WIN64
		int addrLen = sizeof(clientAddr);
#else
		socklen_t addrLen = sizeof(clientAddr);
#endif
		int clientSock = static_cast<int>(accept(_listenSock,
			reinterpret_cast<struct sockaddr*>(&clientAddr), &addrLen));

		if (clientSock < 0)
		{
			if (!_running) break;
			continue;
		}

		// Issue #45: cap concurrent handler threads/sockets. If we are already at the
		// in-flight limit, reply 503 and close THIS connection immediately rather than
		// spawning another thread+socket — a browser hammering parallel XHRs (or a soft
		// DoS) can no longer exhaust the LWIP socket pool out from under SIP/the anchor.
		// Reserve the slot before spawning; the handler releases it on exit.
		if (_activeConns.load(std::memory_order_acquire) >= kMaxConcurrentConns)
		{
			sendResponse(clientSock, 503, "Service Unavailable", "application/json",
			             "{\"error\":\"server busy; too many concurrent connections\"}");
			closeSocket(clientSock);
			continue;
		}
		_activeConns.fetch_add(1, std::memory_order_acq_rel);

		// Dispatch client handling in a detached thread context to prevent DoS connection stalls.
		// std::thread's constructor THROWS std::system_error if the underlying task can't be
		// created (transient heap/task-limit pressure — e.g. the dashboard polling every 2s
		// while SIP traffic churns the heap). An uncaught throw here runs on the accept-loop
		// pthread and calls std::terminate()/abort(), rebooting the whole device. Catch it and
		// drop just this one connection so the server keeps serving instead of crashing.
		try
		{
			std::thread([this, clientSock]() {
				handleClient(clientSock);
				// Release the in-flight slot once this handler is fully done.
				_activeConns.fetch_sub(1, std::memory_order_acq_rel);
			}).detach();
		}
		catch (const std::exception& e)
		{
			// Spawn failed: we never entered handleClient, so release the slot here.
			_activeConns.fetch_sub(1, std::memory_order_acq_rel);
			std::cerr << "[HttpServer] connection thread spawn failed: " << e.what()
				<< " — dropping connection\n";
#if defined _WIN32 || defined _WIN64
			closesocket(clientSock);
#else
			close(clientSock);
#endif
			// Brief backoff so we don't spin-fail under sustained memory pressure.
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}
}

void HttpServer::handleClient(int clientSock)
{
	// Issue #23 resolved: Added SO_RCVTIMEO per-client socket timeout and capped Content-Length to 16KB to prevent Accept thread DoS
#if defined _WIN32 || defined _WIN64
	DWORD tv = 5000; // 5 seconds timeout
	setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
	struct timeval tv{ .tv_sec = 5, .tv_usec = 0 };
	setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

	// Heap-allocate the read buffer. On ESP32 the accept loop runs on a std::thread
	// whose default pthread stack is ~3 KB; a 4 KB stack-local buffer would overflow
	// on the first request. Using std::vector keeps the data on the heap.
	std::vector<char> buf(4096, 0);

	// Read initial data. A follow-up loop below handles POST bodies that span
	// multiple TCP segments (see Content-Length body-read completion below).
#if defined _WIN32 || defined _WIN64
	int bytesRead = recv(clientSock, buf.data(), static_cast<int>(buf.size()) - 1, 0);
#else
	int bytesRead = static_cast<int>(recv(clientSock, buf.data(), buf.size() - 1, 0));
#endif

	if (bytesRead <= 0)
	{
		closeSocket(clientSock);
		return;
	}

	// #18: ensure the complete POST body is present before parsing.
	// If the headers indicate a Content-Length larger than what arrived in the
	// first segment, keep reading until we have it all.
	std::string raw(buf.data(), static_cast<size_t>(bytesRead));

	// --- OTA upload interception (firmware streaming) -------------------------
	// A firmware image is >1.5 MB, so it must NOT flow through the 16 KB-capped
	// buffered path below. As soon as we have the request line + full header
	// block in the first recv, detect "POST /api/ota/upload" and hand off to the
	// streaming handler, which drains the body in fixed chunks. We require the
	// header terminator (\r\n\r\n) to be present in this first segment — it is
	// for any real HTTP client (headers are a few hundred bytes, the 4 KB recv
	// covers them; the multi-MB part is the body, which we stream).
	{
		size_t reqLineEnd = raw.find("\r\n");
		size_t hdrEnd     = raw.find("\r\n\r\n");
		if (reqLineEnd != std::string::npos && hdrEnd != std::string::npos)
		{
			// Cheap method+path probe on the request line only.
			const std::string reqLine = raw.substr(0, reqLineEnd);
			if (reqLine.compare(0, 5, "POST ") == 0 &&
			    reqLine.find(" /api/ota/upload ") != std::string::npos)
			{
				// Parse just the header block (parseRequest tolerates a truncated
				// body) to get method/path/origin/host/cookie for the auth gate.
				HttpRequest otaReq = parseRequest(raw.substr(0, hdrEnd + 4));

				// Same-origin + (provisioned ? authed) gate — identical policy to
				// the other mutating endpoints.
				if (!isSameOrigin(otaReq))
				{
					sendResponse(clientSock, 403, "Forbidden", "application/json",
					             "{\"error\":\"cross-origin request rejected\"}");
					closeSocket(clientSock);
					return;
				}
				if (AdminAuth::isProvisioned() && !isAuthed(otaReq))
				{
					sendResponse(clientSock, 401, "Unauthorized", "application/json",
					             "{\"error\":\"authentication required\"}");
					closeSocket(clientSock);
					return;
				}

				// Parse Content-Length WITHOUT the 16 KB cap (firmware is large).
				size_t otaLen = 0;
				size_t cl = raw.find("Content-Length:");
				if (cl == std::string::npos) cl = raw.find("content-length:");
				if (cl != std::string::npos && cl < hdrEnd)
				{
					size_t p = cl + 15;
					while (p < hdrEnd && std::isspace(static_cast<unsigned char>(raw[p]))) ++p;
					while (p < hdrEnd && std::isdigit(static_cast<unsigned char>(raw[p])))
					{
						// Clamp to a sane ceiling (32 MB > 16 MB flash) to bound work.
						if (otaLen > 32u * 1024u * 1024u) { otaLen = 32u * 1024u * 1024u; break; }
						otaLen = otaLen * 10 + static_cast<size_t>(raw[p] - '0');
						++p;
					}
				}
				if (otaLen == 0)
				{
					sendResponse(clientSock, 411, "Length Required", "application/json",
					             "{\"error\":\"OTA upload requires a non-zero Content-Length\"}");
					closeSocket(clientSock);
					return;
				}

				// Issue #47: pull the detached image signature from the
				// X-OTA-Signature header (base64 of the raw 64-byte ECDSA-P256 r||s
				// signature over the image SHA-256), parsed case-insensitively from
				// the header block only, then base64-decoded. Absent/malformed ->
				// empty (handleOtaUpload enforces the signatureRequired() policy).
				std::string otaSigDer;
				{
					std::string hdrBlock = raw.substr(0, hdrEnd);
					std::string lower = hdrBlock;
					std::transform(lower.begin(), lower.end(), lower.begin(),
					               [](unsigned char ch){ return static_cast<char>(std::tolower(ch)); });
					size_t hp = lower.find("\nx-ota-signature:");
					size_t nameLen = std::strlen("\nx-ota-signature:");
					if (hp == std::string::npos && lower.compare(0, std::strlen("x-ota-signature:"), "x-ota-signature:") == 0)
					{
						hp = 0;                          // header is the very first line
						nameLen = std::strlen("x-ota-signature:");
					}
					else if (hp != std::string::npos)
					{
						hp += 1;                         // step over the matched '\n'
						nameLen -= 1;
					}
					if (hp != std::string::npos)
					{
						size_t vs = hp + nameLen;
						size_t ve = hdrBlock.find_first_of("\r\n", vs);
						if (ve == std::string::npos) ve = hdrBlock.size();
						std::string b64 = hdrBlock.substr(vs, ve - vs);
						// Trim surrounding whitespace.
						size_t a = b64.find_first_not_of(" \t");
						size_t b = b64.find_last_not_of(" \t");
						if (a != std::string::npos)
							otaSigDer = base64Decode(b64.substr(a, b - a + 1));
					}
				}

				handleOtaUpload(clientSock, raw, hdrEnd + 4, otaLen, otaSigDer);
				closeSocket(clientSock);
				return;
			}
		}
	}
	// --- end OTA interception -------------------------------------------------

	size_t clPos = raw.find("Content-Length:");
	if (clPos == std::string::npos)
		clPos = raw.find("content-length:");
	if (clPos != std::string::npos)
	{
		size_t valStart = raw.find_first_not_of(" \t", clPos + 15);
		size_t valEnd   = raw.find_first_of("\r\n", valStart);
		if (valStart != std::string::npos && valEnd != std::string::npos)
		{
			size_t contentLength = 0;
			size_t parseIdx = valStart;
			while (parseIdx < valEnd && std::isspace(static_cast<unsigned char>(raw[parseIdx]))) ++parseIdx;
			while (parseIdx < valEnd && std::isdigit(static_cast<unsigned char>(raw[parseIdx])))
			{
				if (contentLength > 200000000)
				{
					contentLength = 200000000;
					break;
				}
				contentLength = contentLength * 10 + (raw[parseIdx] - '0');
				++parseIdx;
			}

			// Cap total body we are willing to read (16 KB is generous for wifi passwords)
			constexpr size_t MAX_BODY_BYTES = 16384;
			if (contentLength > MAX_BODY_BYTES)
			{
				sendResponse(clientSock, 413, "Payload Too Large", "application/json",
				             "{\"error\":\"request body exceeds 16 KB limit\"}");
				closeSocket(clientSock);
				return;
			}

			size_t headerEnd = raw.find("\r\n\r\n");
			if (headerEnd != std::string::npos)
			{
				size_t bodyStart  = headerEnd + 4;
				size_t bodyHave   = raw.size() > bodyStart ? raw.size() - bodyStart : 0;
				while (bodyHave < contentLength)
				{
					buf.assign(buf.size(), 0);
#if defined _WIN32 || defined _WIN64
					int n = recv(clientSock, buf.data(), static_cast<int>(buf.size()) - 1, 0);
#else
					int n = static_cast<int>(recv(clientSock, buf.data(), buf.size() - 1, 0));
#endif
					if (n <= 0) break;
					raw.append(buf.data(), static_cast<size_t>(n));
					bodyHave += static_cast<size_t>(n);
				}
			}
		}
	}

	HttpRequest req = parseRequest(raw);

#if defined(ESP_PLATFORM)
	// Captive Portal Redirect: If the request is a GET, and the Host is not our IP or is a generic captive portal test domain,
	// redirect the user to our landing page. This triggers the OS captive portal prompt.
	bool isLocalHost = (req.host.find("192.168.4.1") != std::string::npos || 
	                    req.host.find("localhost") != std::string::npos ||
	                    req.host.find("pocketdial") != std::string::npos ||
	                    _ip == "0.0.0.0" || 
	                    req.host.find(_ip) != std::string::npos);

	if (req.method == "GET" && !isLocalHost && req.path != "/api/status" && req.path != "/api/wifi/scan")
	{
		sendRedirect(clientSock, "http://192.168.4.1/");
		closeSocket(clientSock);
		return;
	}
#endif

	// Route
	if (req.method == "GET" && (req.path == "/" || req.path == "/index.html"))
	{
		sendHtml(clientSock);
	}
	else if (req.method == "GET" && req.path == "/api/status")
	{
		sendApiStatus(clientSock);
	}
	else if (req.method == "POST" && req.path == "/api/kill")
	{
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else if (AdminAuth::isProvisioned() && !isAuthed(req))
		{
			sendResponse(clientSock, 401, "Unauthorized", "application/json",
			             "{\"error\":\"authentication required\"}");
		}
		else
		{
			sendApiKill(clientSock, req.body);
		}
	}
	else if (req.method == "GET" && req.path == "/api/cdr")
	{
		// Read-only Call Detail Records — ungated like /api/status.
		sendApiCdr(clientSock);
	}
	else if (req.method == "POST" && req.path == "/api/dnd")
	{
		// Mutating: same gate as /api/kill (same-origin + auth once provisioned).
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else if (AdminAuth::isProvisioned() && !isAuthed(req))
		{
			sendResponse(clientSock, 401, "Unauthorized", "application/json",
			             "{\"error\":\"authentication required\"}");
		}
		else
		{
			sendApiDnd(clientSock, req.body);
		}
	}
	else if (req.method == "POST" && req.path == "/api/forward")
	{
		// Mutating: same gate as /api/dnd (same-origin + auth once provisioned).
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else if (AdminAuth::isProvisioned() && !isAuthed(req))
		{
			sendResponse(clientSock, 401, "Unauthorized", "application/json",
			             "{\"error\":\"authentication required\"}");
		}
		else
		{
			sendApiForward(clientSock, req.body);
		}
	}
	else if (req.method == "POST" && req.path == "/api/group")
	{
		// Mutating: same gate as /api/dnd (same-origin + auth once provisioned).
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else if (AdminAuth::isProvisioned() && !isAuthed(req))
		{
			sendResponse(clientSock, 401, "Unauthorized", "application/json",
			             "{\"error\":\"authentication required\"}");
		}
		else
		{
			sendApiGroup(clientSock, req.body);
		}
	}
	else if (req.method == "GET" && req.path == "/api/wifi/scan")
	{
		sendApiWifiScan(clientSock);
	}
	else if (req.method == "POST" && req.path == "/api/wifi/connect")
	{
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else if (AdminAuth::isProvisioned() && !isAuthed(req))
		{
			sendResponse(clientSock, 401, "Unauthorized", "application/json",
			             "{\"error\":\"authentication required\"}");
		}
		else
		{
			sendApiWifiConnect(clientSock, req.body);
		}
	}
	else if (req.method == "POST" && req.path == "/api/wifi/mode_ap")
	{
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else if (AdminAuth::isProvisioned() && !isAuthed(req))
		{
			sendResponse(clientSock, 401, "Unauthorized", "application/json",
			             "{\"error\":\"authentication required\"}");
		}
		else
		{
			sendApiWifiModeAp(clientSock);
		}
	}
	else if (req.method == "POST" && req.path == "/api/configuring")
	{
		// "I'm configuring" — hold the captive-portal decay so it doesn't switch to
		// Standalone out from under the user. Harmless, so no same-origin gate.
		sendApiConfiguring(clientSock);
	}
	else if (req.method == "POST" && req.path == "/api/factory-reset")
	{
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else if (AdminAuth::isProvisioned() && !isAuthed(req))
		{
			sendResponse(clientSock, 401, "Unauthorized", "application/json",
			             "{\"error\":\"authentication required\"}");
		}
		else
		{
			sendApiFactoryReset(clientSock, req.body);
		}
	}
	else if (req.method == "GET" && req.path == "/api/admin/status")
	{
		// Read-only: tells the dashboard whether to show a set-PIN or login form.
		sendApiAdminStatus(clientSock, req);
	}
	else if (req.method == "POST" && req.path == "/api/admin/set-pin")
	{
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else
		{
			sendApiAdminSetPin(clientSock, req);
		}
	}
	else if (req.method == "POST" && req.path == "/api/admin/login")
	{
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else
		{
			sendApiAdminLogin(clientSock, req);
		}
	}
	else if (req.method == "POST" && req.path == "/api/admin/logout")
	{
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else
		{
			sendApiAdminLogout(clientSock, req);
		}
	}
	else if (req.method == "GET" && req.path == "/api/ota/status")
	{
		// Read-only OTA introspection (partition labels + pending flag). No
		// secrets, so it's readable like /api/status — safe pre-auth.
		sendApiOtaStatus(clientSock);
	}
	else if (req.method == "POST" && req.path == "/api/ota/reboot")
	{
		if (!isSameOrigin(req))
		{
			sendResponse(clientSock, 403, "Forbidden", "application/json",
			             "{\"error\":\"cross-origin request rejected\"}");
		}
		else if (AdminAuth::isProvisioned() && !isAuthed(req))
		{
			sendResponse(clientSock, 401, "Unauthorized", "application/json",
			             "{\"error\":\"authentication required\"}");
		}
		else
		{
			sendApiOtaReboot(clientSock);
		}
	}
	// NOTE: POST /api/ota/upload is handled earlier in handleClient() via the
	// streaming interception (it must bypass the 16 KB buffered body path), so
	// it deliberately does NOT appear in this route table.
	else
	{
		send404(clientSock);
	}

	closeSocket(clientSock);
}

HttpServer::HttpRequest HttpServer::parseRequest(const std::string& raw)
{
	HttpRequest req;

	// Parse request line: "GET /path HTTP/1.1\r\n"
	size_t methodEnd = raw.find(' ');
	if (methodEnd == std::string::npos) return req;
	req.method = raw.substr(0, methodEnd);

	size_t pathStart = methodEnd + 1;
	size_t pathEnd = raw.find(' ', pathStart);
	if (pathEnd == std::string::npos) return req;
	req.path = raw.substr(pathStart, pathEnd - pathStart);

	// Strip query string
	size_t queryPos = req.path.find('?');
	if (queryPos != std::string::npos)
	{
		req.path = req.path.substr(0, queryPos);
	}

	// Efficient, low-overhead line-by-line header scanner (Observation 1)
	size_t pos = raw.find("\r\n");
	if (pos != std::string::npos) {
		pos += 2; // Skip request line
		while (pos < raw.size()) {
			size_t lineEnd = raw.find("\r\n", pos);
			if (lineEnd == std::string::npos) break;
			if (lineEnd == pos) break; // Reached header-body boundary

			std::string line = raw.substr(pos, lineEnd - pos);
			size_t colon = line.find(':');
			if (colon != std::string::npos) {
				std::string hName = line.substr(0, colon);
				std::transform(hName.begin(), hName.end(), hName.begin(), ::tolower);
				
				// Strip trailing whitespaces from name
				while (!hName.empty() && std::isspace(static_cast<unsigned char>(hName.back()))) hName.pop_back();

				if (hName == "origin" || hName == "host" || hName == "cookie") {
					size_t valStart = colon + 1;
					while (valStart < line.size() && std::isspace(static_cast<unsigned char>(line[valStart]))) valStart++;
					std::string hVal = line.substr(valStart);
					while (!hVal.empty() && std::isspace(static_cast<unsigned char>(hVal.back()))) hVal.pop_back();
					if (hName == "origin") req.origin = hVal;
					else if (hName == "host") req.host = hVal;
					else if (hName == "cookie") req.cookie = hVal;
				}
			}
			pos = lineEnd + 2;
		}
	}

	// Find body (after \r\n\r\n)
	size_t bodyStart = raw.find("\r\n\r\n");
	if (bodyStart != std::string::npos)
	{
		req.body = raw.substr(bodyStart + 4);
	}

	return req;
}

void HttpServer::sendResponseWithHeader(int sock, int statusCode, const std::string& statusText,
                              const std::string& contentType, const std::string& body,
                              const std::string& extraHeader)
{
	std::ostringstream resp;
	resp << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
	resp << "Content-Type: " << contentType << "\r\n";
	resp << "Content-Length: " << body.size() << "\r\n";
	// No Access-Control-Allow-Origin header: wildcard CORS would allow any
	// browser tab on the same AP to fire side-effecting POSTs without a preflight.
	if (!extraHeader.empty())
	{
		resp << extraHeader << "\r\n";
	}
	resp << "Connection: close\r\n";
	resp << "\r\n";
	resp << body;

	std::string data = resp.str();
	const char* ptr = data.c_str();
	size_t remaining = data.size();
	while (remaining > 0)
	{
#if defined _WIN32 || defined _WIN64
		int sent = ::send(sock, ptr, static_cast<int>(remaining), 0);
#else
		int sent = static_cast<int>(::send(sock, ptr, remaining, 0));
#endif
		if (sent <= 0) break;
		ptr += sent;
		remaining -= static_cast<size_t>(sent);
	}
}

void HttpServer::sendResponse(int sock, int statusCode, const std::string& statusText,
                              const std::string& contentType, const std::string& body)
{
	sendResponseWithHeader(sock, statusCode, statusText, contentType, body, "");
}

void HttpServer::sendHtml(int sock)
{
	sendResponse(sock, 200, "OK", "text/html; charset=utf-8",
	             std::string(CGA_INDEX_HTML));
}

// Helper: standard base64 decode (RFC 4648). Returns the decoded bytes; on any
// invalid character (other than '=' padding and ASCII whitespace, which are skipped)
// returns an EMPTY string so a malformed X-OTA-Signature is treated as "no
// signature" rather than silently truncated. Pure / host-compilable (used by the
// OTA signature path, issue #47).
static std::string base64Decode(const std::string& in)
{
	auto val = [](unsigned char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+') return 62;
		if (c == '/') return 63;
		return -1;
	};
	std::string out;
	out.reserve((in.size() / 4) * 3 + 3);
	int acc = 0, bits = 0;
	for (unsigned char c : in)
	{
		if (c == '=' ) break;                       // padding -> end of data
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
		int v = val(c);
		if (v < 0) return std::string();            // invalid char -> reject whole input
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			out.push_back(static_cast<char>((acc >> bits) & 0xFF));
		}
	}
	return out;
}

// Helper: JSON-escape a string
static std::string jsonEscape(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s)
	{
		switch (c)
		{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:   out += c;
		}
	}
	return out;
}

void HttpServer::sendApiStatus(int sock)
{
	uint64_t uptimeMs = currentTimeMs() - _startTime;
	uint64_t uptimeSec = uptimeMs / 1000;

	std::vector<std::pair<std::string, std::string>> clients;
	std::vector<std::tuple<std::string, std::string, std::string, int>> sessions;
	std::vector<std::string> dndExtensions;
	std::vector<std::tuple<std::string, std::string, std::string, std::string>> forwards;
	std::vector<std::tuple<std::string, std::string, std::string>> ringGroups;
	uint64_t packets = 0;
	uint64_t dropped = 0;
	RequestsHandler::Telemetry telem;   // soak telemetry (#81/#83): anchor/media/pool health

	RequestsHandler* handler = _handler.load(std::memory_order_acquire);
	if (handler != nullptr)
	{
		clients = handler->getActiveClients();
		sessions = handler->getActiveSessions();
		dndExtensions = handler->getDndExtensions();
		forwards = handler->getForwards();
		ringGroups = handler->getRingGroups();
		packets = handler->getPacketsProcessed();
		dropped = handler->getPacketsDropped();   // Issue #38
		telem = handler->getTelemetry();
	}

	std::string displayIp = _ip;
	if (displayIp == "0.0.0.0")
	{
		displayIp = getPrimaryLocalIP();
	}

	std::ostringstream json;
	json << "{";
	json << "\"ip\":\"" << jsonEscape(displayIp) << "\",";
	json << "\"port\":" << 5060 << ",";
	json << "\"httpPort\":" << _port << ",";
	json << "\"uptime\":" << uptimeSec << ",";
	json << "\"packetsProcessed\":" << packets << ",";
	json << "\"packetsDropped\":" << dropped << ",";

	// Clients array
	json << "\"clients\":[";
	for (size_t i = 0; i < clients.size(); i++)
	{
		if (i > 0) json << ",";
		json << "{\"number\":\"" << jsonEscape(clients[i].first)
		     << "\",\"address\":\"" << jsonEscape(clients[i].second) << "\"}";
	}
	json << "],";

	// Sessions array
	json << "\"sessions\":[";
	for (size_t i = 0; i < sessions.size(); i++)
	{
		if (i > 0) json << ",";
		int durationSec = std::get<3>(sessions[i]);
		int hrs = durationSec / 3600;
		int mins = (durationSec % 3600) / 60;
		int secs = durationSec % 60;
		char durationBuf[32]{};
		if (hrs > 0)
		{
			snprintf(durationBuf, sizeof(durationBuf), "%02d:%02d:%02d", hrs, mins, secs);
		}
		else
		{
			snprintf(durationBuf, sizeof(durationBuf), "%02d:%02d", mins, secs);
		}

		json << "{\"caller\":\"" << jsonEscape(std::get<0>(sessions[i]))
		     << "\",\"callee\":\"" << jsonEscape(std::get<1>(sessions[i]))
		     << "\",\"state\":\"" << jsonEscape(std::get<2>(sessions[i]))
		     << "\",\"duration\":\"" << durationBuf << "\"}";
	}
	json << "],";

	// DND array: extensions currently in Do Not Disturb (Phase 2).
	json << "\"dnd\":[";
	for (size_t i = 0; i < dndExtensions.size(); i++)
	{
		if (i > 0) json << ",";
		json << "\"" << jsonEscape(dndExtensions[i]) << "\"";
	}
	json << "],";

	// Call-forward array (Class A sweep): per-extension always/busy/noanswer targets.
	json << "\"forwards\":[";
	for (size_t i = 0; i < forwards.size(); i++)
	{
		if (i > 0) json << ",";
		json << "{\"extension\":\"" << jsonEscape(std::get<0>(forwards[i]))
		     << "\",\"always\":\""   << jsonEscape(std::get<1>(forwards[i]))
		     << "\",\"busy\":\""     << jsonEscape(std::get<2>(forwards[i]))
		     << "\",\"noanswer\":\"" << jsonEscape(std::get<3>(forwards[i])) << "\"}";
	}
	json << "],";

	// Ring/hunt-group array (Class A sweep): group ext, mode, comma-joined members.
	json << "\"groups\":[";
	for (size_t i = 0; i < ringGroups.size(); i++)
	{
		if (i > 0) json << ",";
		json << "{\"extension\":\"" << jsonEscape(std::get<0>(ringGroups[i]))
		     << "\",\"mode\":\""     << jsonEscape(std::get<1>(ringGroups[i]))
		     << "\",\"members\":\""  << jsonEscape(std::get<2>(ringGroups[i])) << "\"}";
	}
	json << "]";

	// ── Soak telemetry (issues #81-84) ───────────────────────────────────────────
	// anchor/media/pool from the handler (lock-free), heap/PSRAM/reset-reason from the
	// system (ESP only — host build emits 0 / "host" so the schema is stable everywhere).
	json << ",\"telemetry\":{";
	json << "\"anchorConnected\":" << (telem.anchorConnected ? "true" : "false") << ",";
	json << "\"mediaActive\":"     << (telem.mediaActive ? "true" : "false") << ",";
	json << "\"tlsSocketsEst\":"   << telem.tlsSocketsEst << ",";
	json << "\"tlsFullHandshakes\":"    << telem.tlsFullHandshakes << ",";
	json << "\"tlsResumedHandshakes\":" << telem.tlsResumedHandshakes << ",";
	json << "\"playoutUnderruns\":" << telem.playoutUnderruns << ",";
	json << "\"playoutOverruns\":"  << telem.playoutOverruns << ",";
	json << "\"clientPool\":{\"used\":" << telem.clientsUsed << ",\"cap\":" << telem.clientsCap << "},";
	json << "\"sessionPool\":{\"used\":" << telem.sessionsUsed << ",\"cap\":" << telem.sessionsCap << "},";

	uint64_t freeHeap = 0, minFreeHeap = 0, psramFree = 0, psramTotal = 0;
	const char* resetReason = "host";
#if defined(ESP_PLATFORM)
	freeHeap    = esp_get_free_heap_size();
	minFreeHeap = esp_get_minimum_free_heap_size();   // low-water since boot
	psramFree   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	psramTotal  = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
	switch (esp_reset_reason())
	{
		case ESP_RST_POWERON:   resetReason = "poweron";   break;
		case ESP_RST_SW:        resetReason = "sw";        break;
		case ESP_RST_PANIC:     resetReason = "panic";     break;
		case ESP_RST_INT_WDT:   resetReason = "int_wdt";   break;
		case ESP_RST_TASK_WDT:  resetReason = "task_wdt";  break;
		case ESP_RST_WDT:       resetReason = "wdt";       break;
		case ESP_RST_BROWNOUT:  resetReason = "brownout";  break;
		case ESP_RST_DEEPSLEEP: resetReason = "deepsleep"; break;
		case ESP_RST_EXT:       resetReason = "ext";       break;
		case ESP_RST_CPU_LOCKUP: resetReason = "cpu_lockup"; break;  // crash-relevant for soak
		case ESP_RST_PWR_GLITCH: resetReason = "pwr_glitch"; break;
		case ESP_RST_USB:       resetReason = "usb";       break;   // flash/serial-tool reset
		case ESP_RST_JTAG:      resetReason = "jtag";      break;
		case ESP_RST_SDIO:      resetReason = "sdio";      break;
		case ESP_RST_EFUSE:     resetReason = "efuse";     break;
		default:                resetReason = "unknown";   break;
	}
#endif
	json << "\"freeHeap\":" << freeHeap << ",";
	json << "\"minFreeHeap\":" << minFreeHeap << ",";
	json << "\"psramFree\":" << psramFree << ",";
	json << "\"psramTotal\":" << psramTotal << ",";
	json << "\"resetReason\":\"" << jsonEscape(resetReason) << "\"";
	json << "}";

	json << "}";

	sendResponse(sock, 200, "OK", "application/json", json.str());
}

void HttpServer::sendApiKill(int sock, const std::string& body)
{
	// Parse "extension=XXXX" from the body
	std::string ext;
	std::string prefix = "extension=";
	size_t pos = body.find(prefix);
	if (pos != std::string::npos)
	{
		ext = body.substr(pos + prefix.size());
		// Trim whitespace / newlines
		while (!ext.empty() && (ext.back() == '\r' || ext.back() == '\n' || ext.back() == ' '))
			ext.pop_back();
	}

	if (ext.empty())
	{
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"missing extension parameter\"}");
		return;
	}

	if (RequestsHandler* handler = _handler.load(std::memory_order_acquire))
	{
		handler->forceDisconnect(ext);
	}
	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"disconnected\":\"" + jsonEscape(ext) + "\"}");
}

void HttpServer::sendApiCdr(int sock)
{
	std::vector<CallDetailRecord> records;
	if (RequestsHandler* handler = _handler.load(std::memory_order_acquire))
	{
		records = handler->getCallDetailRecords();   // newest first, thread-safe copy
	}

	uint64_t nowMs = currentTimeMs();   // same steady-clock basis as the CDR startMs

	std::ostringstream json;
	json << "[";
	for (size_t i = 0; i < records.size(); i++)
	{
		if (i > 0) json << ",";
		const CallDetailRecord& r = records[i];
		// "ageSec": seconds since the call started, derived from the shared
		// steady-clock basis (no wall clock / RTC is guaranteed on the device).
		uint64_t ageSec = (nowMs >= r.startMs) ? (nowMs - r.startMs) / 1000 : 0;
		json << "{\"caller\":\"" << jsonEscape(r.caller) << "\","
		     << "\"callee\":\"" << jsonEscape(r.callee) << "\","
		     << "\"startMs\":" << r.startMs << ","
		     << "\"ageSec\":" << ageSec << ","
		     << "\"duration\":" << r.durationSec << ","
		     << "\"result\":\"" << cdrResultToString(r.result) << "\"}";
	}
	json << "]";

	sendResponse(sock, 200, "OK", "application/json", json.str());
}

void HttpServer::sendApiDnd(int sock, const std::string& body)
{
	std::string ext = getFormParam(body, "extension");
	std::string on  = getFormParam(body, "on");

	if (ext.empty())
	{
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"missing extension parameter\"}");
		return;
	}

	// Reject the virtual extensions: DND must never affect echo (777) or
	// broadcast (999) — they are not real endpoints.
	if (ext == "777" || ext == "999")
	{
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"cannot set DND on a virtual extension\"}");
		return;
	}

	// Accept 1/true/on as enable; anything else (incl. "0") disables.
	bool enable = (on == "1" || on == "true" || on == "on");

	if (RequestsHandler* handler = _handler.load(std::memory_order_acquire))
	{
		handler->setDnd(ext, enable);
	}

	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"extension\":\"" + jsonEscape(ext) +
	             "\",\"dnd\":" + (enable ? "true" : "false") + "}");
}

void HttpServer::sendApiForward(int sock, const std::string& body)
{
	// Params: extension, trigger ("always"|"busy"|"noanswer"), target (empty=clear).
	std::string ext     = getFormParam(body, "extension");
	std::string trigger = getFormParam(body, "trigger");
	std::string target  = getFormParam(body, "target");

	if (ext.empty() || trigger.empty())
	{
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"missing extension or trigger parameter\"}");
		return;
	}
	if (trigger != "always" && trigger != "busy" && trigger != "noanswer")
	{
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"trigger must be always|busy|noanswer\"}");
		return;
	}
	if (ext == "777" || ext == "999")
	{
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"cannot forward a virtual extension\"}");
		return;
	}

	if (RequestsHandler* handler = _handler.load(std::memory_order_acquire))
	{
		handler->setForward(ext, trigger, target);
	}

	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"extension\":\"" + jsonEscape(ext) +
	             "\",\"trigger\":\"" + jsonEscape(trigger) +
	             "\",\"target\":\"" + jsonEscape(target) + "\"}");
}

void HttpServer::sendApiGroup(int sock, const std::string& body)
{
	// Params: extension (group ext), members (comma/space list), mode ("ringall"|"hunt").
	// An empty member list deletes the group.
	std::string ext     = getFormParam(body, "extension");
	std::string members = getFormParam(body, "members");
	std::string mode    = getFormParam(body, "mode");

	if (ext.empty())
	{
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"missing extension parameter\"}");
		return;
	}
	if (ext == "777" || ext == "999")
	{
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"cannot use a reserved extension as a group\"}");
		return;
	}
	if (mode.empty()) mode = "ringall";
	if (mode != "ringall" && mode != "hunt")
	{
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"mode must be ringall|hunt\"}");
		return;
	}

	if (RequestsHandler* handler = _handler.load(std::memory_order_acquire))
	{
		handler->setRingGroup(ext, members, mode);
	}

	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"extension\":\"" + jsonEscape(ext) +
	             "\",\"mode\":\"" + jsonEscape(mode) +
	             "\",\"members\":\"" + jsonEscape(members) + "\"}");
}

bool HttpServer::isSameOrigin(const HttpRequest& req) const
{
	// No Origin header means a direct request (browser nav, curl, etc.) — allow.
	if (req.origin.empty()) return true;

	// Strip the scheme from the Origin (e.g. "http://192.168.4.1:8080" → "192.168.4.1:8080")
	std::string originHost = req.origin;
	size_t schemeEnd = originHost.find("://");
	if (schemeEnd != std::string::npos)
		originHost = originHost.substr(schemeEnd + 3);

	// Strip port numbers from both originHost and req.host for comparison (if any)
	auto stripPort = [](const std::string& h) -> std::string {
		size_t colon = h.find(':');
		if (colon != std::string::npos) return h.substr(0, colon);
		return h;
	};

	std::string cleanOrigin = stripPort(originHost);
	std::string cleanHost = stripPort(req.host);

	// Host header must be our local IP or local mDNS hostname or localhost
	std::string activeIp = (_ip == "0.0.0.0") ? getPrimaryLocalIP() : _ip;
	bool hostValid = (cleanHost == activeIp || 
	                  cleanHost == "192.168.4.1" || 
	                  cleanHost == "pocketdial.local" ||
	                  cleanHost == "localhost" ||
	                  cleanHost == "127.0.0.1");

	return hostValid && (cleanOrigin == cleanHost);
}

std::string HttpServer::cookieValue(const HttpRequest& req, const std::string& name)
{
	// Cookie header is "k1=v1; k2=v2; ...". Find name= as a token boundary.
	const std::string& c = req.cookie;
	if (c.empty()) return "";

	size_t pos = 0;
	while (pos < c.size())
	{
		// Skip leading spaces / separators.
		while (pos < c.size() && (c[pos] == ' ' || c[pos] == ';')) ++pos;
		size_t eq = c.find('=', pos);
		if (eq == std::string::npos) break;
		std::string k = c.substr(pos, eq - pos);
		size_t valStart = eq + 1;
		size_t valEnd = c.find(';', valStart);
		std::string v = (valEnd == std::string::npos)
			? c.substr(valStart)
			: c.substr(valStart, valEnd - valStart);
		// Trim surrounding whitespace from the value.
		while (!v.empty() && (v.front() == ' ')) v.erase(v.begin());
		while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
		if (k == name) return v;
		if (valEnd == std::string::npos) break;
		pos = valEnd + 1;
	}
	return "";
}

bool HttpServer::isAuthed(const HttpRequest& req) const
{
	std::string token = cookieValue(req, "pd_session");
	if (token.empty()) return false;
	return AdminAuth::validateSession(token);
}

void HttpServer::send404(int sock)
{
	sendResponse(sock, 404, "Not Found", "text/plain", "404 Not Found");
}

void HttpServer::closeSocket(int sock)
{
#if defined(__linux__) || defined(ESP_PLATFORM)
	close(sock);
#elif defined _WIN32 || defined _WIN64
	closesocket(sock);
#endif
}

uint64_t HttpServer::currentTimeMs() const
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()
		).count()
	);
}

// URL decoding lives in UrlEncode.hpp as the header-only urlDecode() (host-testable,
// shared with urlEncode, single source of truth for the trailing-%XX bound — audit
// #73). Call sites below use it unqualified.

static std::string getFormParam(const std::string& body, const std::string& key)
{
	std::string prefix = key + "=";
	size_t pos = body.find(prefix);
	if (pos == std::string::npos)
	{
		prefix = "&" + key + "=";
		pos = body.find(prefix);
		if (pos == std::string::npos) return "";
	}
	size_t start = pos + prefix.length();
	size_t end = body.find('&', start);
	std::string val;
	if (end == std::string::npos) {
		val = body.substr(start);
	} else {
		val = body.substr(start, end - start);
	}
	while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) {
		val.pop_back();
	}
	return urlDecode(val);
}

void HttpServer::sendApiWifiScan(int sock)
{
#if defined(POCKETDIAL_HAS_WIFI)
	// Switch mode to AP+STA so we can scan
	wifi_mode_t current_mode;
	if (esp_wifi_get_mode(&current_mode) == ESP_OK) {
		if (current_mode == WIFI_MODE_AP) {
			esp_wifi_set_mode(WIFI_MODE_APSTA);
		}
	}

	wifi_scan_config_t scan_config = {};
	scan_config.show_hidden = true;
	
	esp_err_t err = esp_wifi_scan_start(&scan_config, true);
	if (err != ESP_OK) {
		sendResponse(sock, 500, "Internal Server Error", "application/json", 
		             "{\"error\":\"WiFi scan start failed\",\"code\":" + std::to_string(err) + "}");
		return;
	}

	uint16_t ap_count = 0;
	esp_wifi_scan_get_ap_num(&ap_count);
	
	std::vector<wifi_ap_record_t> ap_records(ap_count);
	if (ap_count > 0) {
		esp_wifi_scan_get_ap_records(&ap_count, ap_records.data());
	}

	std::ostringstream json;
	json << "{\"networks\":[";
	for (uint16_t i = 0; i < ap_count; ++i) {
		if (i > 0) json << ",";
		std::string ssid(reinterpret_cast<char*>(ap_records[i].ssid));
		int rssi = ap_records[i].rssi;
		std::string enc = "OPEN";
		switch (ap_records[i].authmode) {
			case WIFI_AUTH_WEP: enc = "WEP"; break;
			case WIFI_AUTH_WPA_PSK: enc = "WPA"; break;
			case WIFI_AUTH_WPA2_PSK: enc = "WPA2"; break;
			case WIFI_AUTH_WPA_WPA2_PSK: enc = "WPA/WPA2"; break;
			case WIFI_AUTH_WPA2_ENTERPRISE: enc = "WPA2 Enterprise"; break;
			case WIFI_AUTH_WPA3_PSK: enc = "WPA3"; break;
			case WIFI_AUTH_WPA2_WPA3_PSK: enc = "WPA2/WPA3"; break;
			default: break;
		}
		json << "{\"ssid\":\"" << jsonEscape(ssid) << "\",\"rssi\":" << rssi 
		     << ",\"encryption\":\"" << jsonEscape(enc) << "\"}";
	}
	json << "]}";

	sendResponse(sock, 200, "OK", "application/json", json.str());
#else
	sendResponse(sock, 200, "OK", "application/json", 
	             "{\"networks\":[], \"note\":\"WiFi scan not available on desktop\"}");
#endif
}

void HttpServer::sendApiWifiConnect(int sock, const std::string& body)
{
	std::string ssid = getFormParam(body, "ssid");
	std::string password = getFormParam(body, "password");

	if (ssid.empty())
	{
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"missing ssid parameter\"}");
		return;
	}

#if defined(POCKETDIAL_HAS_WIFI)
	// Issue #58 hardening: check every NVS return (CONTRIBUTING_FIRMWARE.md). A
	// silently-failed write here used to still report "saved" and reboot, which
	// could strand the device on a STALE password (old secret left in flash) — both
	// a correctness and a secret-hygiene problem. On any failure, do NOT reboot and
	// surface a 500 so the operator retries instead of losing connectivity.
	nvs_handle_t nvs_handle;
	esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
	if (err != ESP_OK) {
		sendResponse(sock, 500, "Internal Server Error", "application/json",
		             "{\"error\":\"failed to open credential store\"}");
		return;
	}
	bool nvsOk =
		nvs_set_u8(nvs_handle, "wifi_mode", 1) == ESP_OK &&   // 1 = STATION
		nvs_set_str(nvs_handle, "wifi_ssid", ssid.c_str()) == ESP_OK &&
		nvs_set_str(nvs_handle, "wifi_pass", password.c_str()) == ESP_OK &&
		nvs_commit(nvs_handle) == ESP_OK;
	nvs_close(nvs_handle);
	if (!nvsOk) {
		sendResponse(sock, 500, "Internal Server Error", "application/json",
		             "{\"error\":\"failed to persist WiFi credentials\"}");
		return;
	}

	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"message\":\"WiFi credentials saved. Rebooting to Station Mode...\"}");

	// Create a background task to restart after 1 second
	xTaskCreate([](void*) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		esp_restart();
	}, "restart_task", 2048, NULL, 5, NULL);
#else
	(void)password;
	sendResponse(sock, 501, "Not Implemented", "application/json",
	             "{\"error\":\"WiFi connect not available on desktop\"}");
#endif
}

void HttpServer::sendApiWifiModeAp(int sock)
{
#if defined(POCKETDIAL_HAS_WIFI)
	nvs_handle_t nvs_handle;
	esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
	if (err == ESP_OK) {
		nvs_set_u8(nvs_handle, "wifi_mode", 2); // 2 = AP (Standalone)
		nvs_commit(nvs_handle);
		nvs_close(nvs_handle);
	}

	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"message\":\"Operational mode set to Standalone AP. Rebooting...\"}");

	// Create a background task to restart after 1 second
	xTaskCreate([](void*) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		esp_restart();
	}, "restart_task", 2048, NULL, 5, NULL);
#else
	sendResponse(sock, 501, "Not Implemented", "application/json",
	             "{\"error\":\"WiFi mode select not available on desktop\"}");
#endif
}

void HttpServer::sendApiConfiguring(int sock)
{
	// Pause the captive-portal decay watchdog: the user is actively configuring, so don't
	// auto-switch to Standalone. Held until they save a mode or factory-reset (both reboot).
	g_decayHold = true;
	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"message\":\"Setup mode held \\u2014 auto-switch to Standalone paused.\"}");
}

void HttpServer::sendApiFactoryReset(int sock, const std::string& body)
{
	// Require an explicit confirm token so a stray/accidental POST can't wipe the device.
	if (getFormParam(body, "confirm") != "ERASE") {
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"factory reset requires confirm=ERASE\"}");
		return;
	}
	// Clear the admin credential + all sessions so the device returns to the
	// unprovisioned/open state on both ESP (NVS) and host (in-memory).
	AdminAuth::clearCredential();
#if defined(POCKETDIAL_HAS_WIFI)
	nvs_handle_t nvs_handle;
	if (nvs_open("storage", NVS_READWRITE, &nvs_handle) == ESP_OK) {
		nvs_erase_key(nvs_handle, "wifi_mode");
		nvs_erase_key(nvs_handle, "wifi_ssid");
		nvs_erase_key(nvs_handle, "wifi_pass");
		nvs_erase_key(nvs_handle, "decayed");
		nvs_commit(nvs_handle);
		nvs_close(nvs_handle);
	}
	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"message\":\"Factory reset. Rebooting to captive-portal setup...\"}");
	xTaskCreate([](void*) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		esp_restart();
	}, "restart_task", 2048, NULL, 5, NULL);
#else
	sendResponse(sock, 501, "Not Implemented", "application/json",
	             "{\"error\":\"factory reset not available on desktop\"}");
#endif
}

void HttpServer::sendApiAdminStatus(int sock, const HttpRequest& req)
{
	bool provisioned = AdminAuth::isProvisioned();
	bool authenticated = isAuthed(req);
	std::ostringstream json;
	json << "{\"provisioned\":" << (provisioned ? "true" : "false")
	     << ",\"authenticated\":" << (authenticated ? "true" : "false") << "}";
	sendResponse(sock, 200, "OK", "application/json", json.str());
}

void HttpServer::sendApiAdminSetPin(int sock, const HttpRequest& req)
{
	// First-run onboarding: setting a PIN is allowed only when the device is not
	// yet provisioned, OR when the caller already holds a valid session (changing
	// an existing PIN). This prevents an unauthenticated AP peer from overwriting
	// a provisioned admin PIN.
	if (AdminAuth::isProvisioned() && !isAuthed(req))
	{
		sendResponse(sock, 401, "Unauthorized", "application/json",
		             "{\"error\":\"authentication required to change PIN\"}");
		return;
	}

	std::string pin = getFormParam(req.body, "pin");
	if (pin.size() < AdminAuth::kMinPinLength)
	{
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"PIN must be at least 4 characters\"}");
		return;
	}

	if (!AdminAuth::setPin(pin))
	{
		sendResponse(sock, 500, "Internal Server Error", "application/json",
		             "{\"error\":\"failed to store PIN\"}");
		return;
	}

	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"provisioned\":true}");
}

void HttpServer::sendApiAdminLogin(int sock, const HttpRequest& req)
{
	if (!AdminAuth::isProvisioned())
	{
		// Nothing to log in to yet — direct the client to set a PIN first.
		sendResponse(sock, 409, "Conflict", "application/json",
		             "{\"error\":\"no admin PIN set; call /api/admin/set-pin first\"}");
		return;
	}

	// Reject while locked out before doing any hashing work. Issue #57: the HTTP
	// login throttle is now scoped to the Http channel, so SSH/DTMF brute-force no
	// longer locks the dashboard out (and vice-versa).
	if (AdminAuth::isLockedOut(AdminAuth::Channel::Http))
	{
		sendResponse(sock, 429, "Too Many Requests", "application/json",
		             "{\"error\":\"too many failed attempts; try again later\"}");
		return;
	}

	std::string pin = getFormParam(req.body, "pin");
	if (!AdminAuth::verifyPin(pin, AdminAuth::Channel::Http))
	{
		// verifyPin may have just engaged the lockout on this attempt.
		if (AdminAuth::isLockedOut(AdminAuth::Channel::Http))
		{
			sendResponse(sock, 429, "Too Many Requests", "application/json",
			             "{\"error\":\"too many failed attempts; try again later\"}");
		}
		else
		{
			sendResponse(sock, 401, "Unauthorized", "application/json",
			             "{\"error\":\"invalid PIN\"}");
		}
		return;
	}

	std::string token = AdminAuth::createSession();
	if (token.empty())
	{
		sendResponse(sock, 500, "Internal Server Error", "application/json",
		             "{\"error\":\"failed to create session\"}");
		return;
	}

	// HttpOnly: not readable from JS (mitigates XSS token theft).
	// SameSite=Strict: the browser won't attach it to cross-site requests
	// (defense in depth alongside the same-origin check). No Secure flag: the
	// dashboard is plain HTTP on a LAN appliance (see docs/THREAT_MODEL.md).
	std::string cookie = "Set-Cookie: pd_session=" + token +
	                     "; HttpOnly; Path=/; SameSite=Strict";
	sendResponseWithHeader(sock, 200, "OK", "application/json",
	                       "{\"status\":\"ok\",\"authenticated\":true}", cookie);
}

void HttpServer::sendApiAdminLogout(int sock, const HttpRequest& req)
{
	std::string token = cookieValue(req, "pd_session");
	if (!token.empty())
	{
		AdminAuth::destroySession(token);
	}
	// Expire the cookie on the client side too (Max-Age=0).
	std::string cookie = "Set-Cookie: pd_session=; HttpOnly; Path=/; SameSite=Strict; Max-Age=0";
	sendResponseWithHeader(sock, 200, "OK", "application/json",
	                       "{\"status\":\"ok\"}", cookie);
}

bool HttpServer::streamBody(int sock, const char* prefix, size_t prefixLen,
                            size_t contentLength,
                            const std::function<bool(const uint8_t*, size_t)>& chunkSink)
{
	size_t consumed = 0;

	// 1) Feed any body bytes that already arrived in the header recv.
	if (prefixLen > 0)
	{
		size_t take = (prefixLen <= contentLength) ? prefixLen : contentLength;
		if (take > 0 && !chunkSink(reinterpret_cast<const uint8_t*>(prefix), take))
			return false;
		consumed += take;
	}

	// 2) Drain the rest off the socket in fixed 4 KB chunks. Heap-allocated
	//    (the accept-loop thread has a ~3 KB pthread stack on ESP — see
	//    handleClient — so a stack buffer would overflow). The per-socket
	//    SO_RCVTIMEO (5 s, set in handleClient) bounds a stalled sender.
	std::vector<uint8_t> chunk(4096);
	while (consumed < contentLength)
	{
		size_t want = contentLength - consumed;
		if (want > chunk.size()) want = chunk.size();
#if defined _WIN32 || defined _WIN64
		int n = recv(sock, reinterpret_cast<char*>(chunk.data()), static_cast<int>(want), 0);
#else
		int n = static_cast<int>(recv(sock, chunk.data(), want, 0));
#endif
		if (n <= 0)
			return false; // peer closed early or timed out → incomplete body
		if (!chunkSink(chunk.data(), static_cast<size_t>(n)))
			return false;
		consumed += static_cast<size_t>(n);
	}
	return consumed == contentLength;
}

void HttpServer::handleOtaUpload(int sock, const std::string& alreadyRead,
                                 size_t bodyStart, size_t contentLength,
                                 const std::string& otaSignatureDer)
{
	const char*  prefix    = (bodyStart <= alreadyRead.size())
	                             ? alreadyRead.data() + bodyStart : nullptr;
	const size_t prefixLen = (bodyStart <= alreadyRead.size())
	                             ? alreadyRead.size() - bodyStart : 0;

#if defined(ESP_PLATFORM)
	// Device path: stream the body straight into the inactive OTA slot.
	OtaUpdater ota;
	if (!ota.begin(contentLength))
	{
		// Drain the body so the client's send completes and we can reply cleanly
		// instead of resetting the connection mid-upload.
		streamBody(sock, prefix, prefixLen, contentLength,
		           [](const uint8_t*, size_t) { return true; });
		sendResponse(sock, 500, "Internal Server Error", "application/json",
		             "{\"error\":\"ota begin failed: " + jsonEscape(ota.lastError()) + "\"}");
		return;
	}

	bool writeOk = streamBody(sock, prefix, prefixLen, contentLength,
		[&ota](const uint8_t* p, size_t n) { return ota.write(p, n); });

	if (!writeOk)
	{
		ota.abort();
		sendResponse(sock, 400, "Bad Request", "application/json",
		             "{\"error\":\"incomplete upload or flash write failed: "
		             + jsonEscape(ota.lastError()) + "\"}");
		return;
	}

	if (!ota.end())
	{
		// end() already released the handle; report the validation error.
		sendResponse(sock, 422, "Unprocessable Entity", "application/json",
		             "{\"error\":\"image rejected: " + jsonEscape(ota.lastError()) + "\"}");
		return;
	}

	// Issue #47: cryptographic signature gate, BEFORE activate(). esp_ota_end() only
	// checked image magic/checksum — not authenticity — so without this an admin (or
	// a first-run, pre-PIN caller) could stage arbitrary firmware. Policy:
	//   * a signature was supplied         -> it MUST verify, else 422 (never staged);
	//   * none supplied + enforcement ON   -> 422 (signature is mandatory);
	//   * none supplied + enforcement OFF  -> loud warning, allow (preserves
	//                                         onboarding / unprovisioned-key units).
	// Enforcement + the trusted key are build-time provisioned (OtaUpdater.hpp /
	// docs/OTA.md). The durable fix remains Secure Boot v2 (eFuse key) — see report.
	if (!otaSignatureDer.empty())
	{
		if (!ota.verifySignature(otaSignatureDer))
		{
			sendResponse(sock, 422, "Unprocessable Entity", "application/json",
			             "{\"error\":\"image signature rejected: "
			             + jsonEscape(ota.lastError()) + "\"}");
			return;   // image written to the inactive slot but NEVER made bootable
		}
	}
	else if (OtaUpdater::signatureRequired())
	{
		sendResponse(sock, 422, "Unprocessable Entity", "application/json",
		             "{\"error\":\"OTA image signature required (X-OTA-Signature header missing)\"}");
		return;
	}
	else
	{
		ESP_LOGW("HttpServer",
		         "OTA image accepted WITHOUT a signature (enforcement off) — set "
		         "PD_OTA_REQUIRE_SIGNATURE + an OTA signing key to require signing");
	}

	if (!ota.activate())
	{
		sendResponse(sock, 500, "Internal Server Error", "application/json",
		             "{\"error\":\"activate failed: " + jsonEscape(ota.lastError()) + "\"}");
		return;
	}

	std::ostringstream json;
	json << "{\"status\":\"ok\",\"bytes\":" << ota.bytesWritten()
	     << ",\"rebootRequired\":true"
	     << ",\"nextPartition\":\"" << jsonEscape(OtaUpdater::nextUpdatePartitionLabel()) << "\""
	     << ",\"message\":\"image staged; POST /api/ota/reboot to boot it\"}";
	sendResponse(sock, 200, "OK", "application/json", json.str());
#else
	// Host stub: real flashing is impossible off-device. Drain the body (bounded
	// by Content-Length + the 5 s socket timeout) so curl completes cleanly, then
	// return 501. We do NOT simulate success — a 200 here could be mistaken for a
	// real update in tooling/CI.
	(void)contentLength;
	(void)otaSignatureDer;   // host has no flash; signature is verified on-device
	streamBody(sock, prefix, prefixLen, contentLength,
	           [](const uint8_t*, size_t) { return true; });
	sendResponse(sock, 501, "Not Implemented", "application/json",
	             "{\"error\":\"OTA only available on device\"}");
#endif
}

void HttpServer::sendApiOtaStatus(int sock)
{
	std::ostringstream json;
	json << "{";
	json << "\"running\":\""  << jsonEscape(OtaUpdater::runningPartitionLabel())    << "\",";
	json << "\"boot\":\""     << jsonEscape(OtaUpdater::bootPartitionLabel())       << "\",";
	json << "\"next\":\""     << jsonEscape(OtaUpdater::nextUpdatePartitionLabel()) << "\",";
	json << "\"pendingVerify\":" << (OtaUpdater::isPendingVerify() ? "true" : "false") << ",";
#if defined(ESP_PLATFORM)
	json << "\"otaSupported\":true,";
#else
	json << "\"otaSupported\":false,";
#endif
	json << "\"error\":\"\"";
	json << "}";
	sendResponse(sock, 200, "OK", "application/json", json.str());
}

void HttpServer::sendApiOtaReboot(int sock)
{
#if defined(ESP_PLATFORM)
	// Only reboot if there is actually a staged image to boot into; otherwise a
	// stray POST would needlessly bounce the device.
	if (OtaUpdater::bootPartitionLabel() == OtaUpdater::runningPartitionLabel())
	{
		sendResponse(sock, 409, "Conflict", "application/json",
		             "{\"error\":\"no pending OTA image to boot into\"}");
		return;
	}

	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"message\":\"rebooting into the new image...\"}");

	// Defer the restart so the HTTP response flushes first (mirrors the WiFi
	// connect/mode endpoints' delayed-restart pattern).
	xTaskCreate([](void*) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		esp_restart();
	}, "ota_reboot", 2048, NULL, 5, NULL);
#else
	// Host stub: never actually exit the process (the smoke-test harness keeps
	// running). Report a simulated success.
	sendResponse(sock, 200, "OK", "application/json",
	             "{\"status\":\"ok\",\"simulated\":true,"
	             "\"message\":\"reboot is a no-op on the desktop build\"}");
#endif
}

void HttpServer::sendRedirect(int sock, const std::string& location)
{
	std::ostringstream resp;
	resp << "HTTP/1.1 302 Found\r\n";
	resp << "Location: " << location << "\r\n";
	resp << "Content-Length: 0\r\n";
	resp << "Connection: close\r\n\r\n";

	std::string data = resp.str();
	::send(sock, data.c_str(), static_cast<int>(data.size()), 0);
}
