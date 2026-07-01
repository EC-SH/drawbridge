#include "SipServer.hpp"
#include "SipMessageTypes.h"

// mDNS is NO LONGER initialised here (issue #154). It used to be brought up in
// this constructor, which on ESP runs INSIDE the SIP task — i.e. AFTER the
// provisioning gate — so a factory-fresh unit sitting at the gate never
// advertised `drawbridge.local`, breaking the `ssh owner@drawbridge.local`
// onboarding path. Worse, on the eth build (whose entry point already brings
// mDNS up pre-gate) this ctor re-ran `mdns_hostname_set()` once SIP started and
// silently renamed the host back to the default. mDNS is now hoisted into each
// transport's `app_main`, before the provisioning gate. See main/esp_main_*.cpp.

SipServer::SipServer(std::string ip, int port, int httpPort) :
	_socket(ip, port, std::bind(&SipServer::onNewMessage, this, std::placeholders::_1, std::placeholders::_2)),
	_handler(ip, port, std::bind(&SipServer::onHandled, this, std::placeholders::_1, std::placeholders::_2))
{
	(void)httpPort;
	_socket.startReceive();

	// ── Desktop Background Tick Thread ───────────────────────────────
#if !defined(ESP_PLATFORM) && !defined(ESP32)
	_tickRunning = true;
	_tickThread = std::thread(&SipServer::tickLoop, this);
#endif
}

SipServer::~SipServer()
{
#if !defined(ESP_PLATFORM) && !defined(ESP32)
	if (_tickRunning)
	{
		_tickRunning = false;
		if (_tickThread.joinable())
		{
			_tickThread.join();
		}
	}
#endif
}

void SipServer::onNewMessage(std::string data, sockaddr_in src)
{
	auto message = _messagesFactory.createMessage(std::move(data), std::move(src));
	if (message.has_value())
	{
		_handler.handle(std::move(message.value()));
	}
}

void SipServer::onHandled(const sockaddr_in& dest, std::shared_ptr<SipMessage> message)
{
	_socket.send(dest, message->toString());
}

#if !defined(ESP_PLATFORM) && !defined(ESP32)
void SipServer::tickLoop()
{
	while (_tickRunning)
	{
		_handler.tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}
}
#endif
