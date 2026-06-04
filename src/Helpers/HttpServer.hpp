#ifndef HTTP_SERVER_HPP
#define HTTP_SERVER_HPP

#if defined(__linux__) || defined(ESP_PLATFORM)
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <cstdint>

// Forward declaration — the HttpServer queries the SIP engine via this
class RequestsHandler;

class HttpServer
{
public:
	HttpServer(const std::string& ip, int port, RequestsHandler* handler = nullptr);
	~HttpServer();

	void start();

private:
	void acceptLoop();
	void handleClient(int clientSock);

	// HTTP request parsing
	struct HttpRequest {
		std::string method;
		std::string path;
		std::string body;
		std::string origin;  // value of the Origin: header, if present
		std::string host;    // value of the Host: header, if present
		std::string cookie;  // value of the Cookie: header, if present
	};
	HttpRequest parseRequest(const std::string& raw);

	// Response builders
	void sendResponse(int sock, int statusCode, const std::string& statusText,
	                   const std::string& contentType, const std::string& body);
	// Same as sendResponse, but injects an extra raw header line (e.g.
	// "Set-Cookie: pd_session=...; HttpOnly; Path=/; SameSite=Strict"). The
	// extraHeader must NOT include the trailing CRLF.
	void sendResponseWithHeader(int sock, int statusCode, const std::string& statusText,
	                   const std::string& contentType, const std::string& body,
	                   const std::string& extraHeader);
	void sendRedirect(int sock, const std::string& location);
	void sendHtml(int sock);
	void sendApiStatus(int sock);
	void sendApiKill(int sock, const std::string& body);
	void sendApiWifiScan(int sock);
	void sendApiWifiConnect(int sock, const std::string& body);
	void sendApiWifiModeAp(int sock);
	void sendApiConfiguring(int sock);
	void sendApiFactoryReset(int sock, const std::string& body);
	void send404(int sock);

	// --- Admin auth endpoints (PIN-gated session layer; see AdminAuth.hpp) ---
	void sendApiAdminStatus(int sock, const HttpRequest& req);
	void sendApiAdminSetPin(int sock, const HttpRequest& req);
	void sendApiAdminLogin(int sock, const HttpRequest& req);
	void sendApiAdminLogout(int sock, const HttpRequest& req);

	// Returns true if the request has no Origin header (direct browser nav / curl)
	// or if the Origin host matches the Host header (same-origin). Blocks CSRF from
	// third-party pages on the same AP.
	bool isSameOrigin(const HttpRequest& req) const;

	// Extract the value of a named cookie (e.g. "pd_session") from the request's
	// Cookie header, or "" if absent.
	static std::string cookieValue(const HttpRequest& req, const std::string& name);

	// True iff the request carries a valid (live) pd_session cookie. Used to gate
	// the state-changing endpoints once a PIN has been provisioned.
	bool isAuthed(const HttpRequest& req) const;

	// Close a client socket portably
	void closeSocket(int sock);

	std::string _ip;
	int _port;
	int _listenSock;
	RequestsHandler* _handler;
	std::atomic<bool> _running;
	std::thread _acceptThread;

	// Track server uptime
	uint64_t _startTime;
	uint64_t currentTimeMs() const;
};

#endif
