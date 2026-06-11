#ifndef THREECX_ANCHOR_CLIENT_HPP
#define THREECX_ANCHOR_CLIENT_HPP

#include "AnchorClient.hpp"
#include <mutex>
#include <string>
#include <atomic>
#include <functional>

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include "esp_websocket_client.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

class ThreeCxAnchorClient : public AnchorClient
{
public:
	ThreeCxAnchorClient();
	~ThreeCxAnchorClient() override;

	bool init(const std::string& baseUrl,
	          const std::string& clientId,
	          const std::string& clientSecret,
	          const std::string& sourceDn) override;

	bool start() override;
	void stop() override;
	bool isConnected() const override;
	bool isStreaming() const;
	bool makeCall(const std::string& destination) override;
	bool dropCall(const std::string& participantId) override;
	void setEventCallback(EventCallback cb) override;
	bool writeAudio(const int16_t* pcmSamples, size_t count) override;
	void registerAudioRxCallback(AudioRxCallback cb) override;

private:
	std::string _baseUrl;
	std::string _clientId;
	std::string _clientSecret;
	std::string _sourceDn;

	std::atomic<bool> _running{false};
	std::atomic<bool> _connected{false};
	std::string       _accessToken;
	std::string       _activeParticipantId;

	EventCallback   _eventCb;
	AudioRxCallback _audioCb;

	mutable std::mutex _mutex;
	std::mutex         _postMutex; // guards _postClient across writeAudio / stopMediaStreams
	std::mutex         _getMutex;  // guards _getClient lifecycle across runRxLoop / stopMediaStreams

	// Token lifetime measured against the monotonic timer (not wall clock, so no
	// SNTP dependency). Derived from the JWT's own exp/iat claims, NOT from the
	// OAuth expires_in field — 3CX reports expires_in:60 but the JWT is valid ~1h,
	// and re-issuing a token invalidates the one the active media streams hold.
	int64_t _tokenObtainedUs = 0;
	int64_t _tokenLifetimeUs = 0;

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	esp_websocket_client_handle_t _wsClient   = nullptr;
	esp_http_client_handle_t      _postClient = nullptr;
	esp_http_client_handle_t      _getClient  = nullptr;

	TaskHandle_t      _rxTaskHandle = nullptr;
	SemaphoreHandle_t _rxDoneSem    = nullptr; // signalled by rx task before it deletes itself

	static void wsEventTrampoline(void* handlerArgs, esp_event_base_t base, int32_t eventId, void* eventData);
	void handleWsEvent(int32_t eventId, void* eventData);

	bool fetchToken();
	bool ensureToken();             // refresh iff expiring AND no media streams active
	bool tokenExpiringSoon() const; // true when within the refresh margin of JWT exp
	bool connectWs();
	std::string getParticipantStatus(const std::string& participantId);

	static void rxTaskTrampoline(void* arg);
	void runRxLoop();

	bool startMediaStreams(const std::string& participantId);
	void stopMediaStreams();

	esp_http_client_handle_t makeAuthedClient(const std::string& url, esp_http_client_method_t method, int txBufSize, const std::string& token = "");
	bool performAuthedRequest(esp_http_client_handle_t client, int* statusCodeOut = nullptr);
	bool readJsonStringField(esp_http_client_handle_t client, const std::string& field, std::string& out);
#endif
};

#endif // THREECX_ANCHOR_CLIENT_HPP
