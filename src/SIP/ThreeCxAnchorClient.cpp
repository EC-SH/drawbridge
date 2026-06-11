#include "ThreeCxAnchorClient.hpp"

#if !defined(ESP_PLATFORM) && !defined(ESP32) && !defined(ARDUINO)

// ── Host stubs: compile-compatible no-ops for host tests ──────────────────
ThreeCxAnchorClient::ThreeCxAnchorClient() = default;
ThreeCxAnchorClient::~ThreeCxAnchorClient() = default;

bool ThreeCxAnchorClient::init(const std::string&, const std::string&, const std::string&, const std::string&)
{
	return false;
}

bool ThreeCxAnchorClient::start()
{
	return false;
}

void ThreeCxAnchorClient::stop()
{
}

bool ThreeCxAnchorClient::isConnected() const
{
	return false;
}

bool ThreeCxAnchorClient::isStreaming() const
{
	return false;
}

bool ThreeCxAnchorClient::makeCall(const std::string&)
{
	return false;
}

bool ThreeCxAnchorClient::dropCall(const std::string&)
{
	return false;
}

void ThreeCxAnchorClient::setEventCallback(EventCallback)
{
}

bool ThreeCxAnchorClient::writeAudio(const int16_t*, size_t)
{
	return false;
}

void ThreeCxAnchorClient::registerAudioRxCallback(AudioRxCallback)
{
}

#else

// ── ESP-IDF Implementation: real mTLS, WSS and chunked HTTPS streams ─────────
#include "esp_log.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include <sstream>
#include <cstring>
#include <cstdio>
#include <vector>
#include <cstdint>
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

static const char* TAG = "ThreeCxAnchor";

// 3CX Call Control WS event_type is a NUMERIC enum, not a string. From the 3CX
// call-control-examples (server/src/types.ts):
//   enum EventType { Upset, Remove, DTMFstring, PromptPlaybackFinished }
// TypeScript auto-numbers these 0..3. The earlier parser compared event_type to
// the strings "Upset"/"Remove"/"DTMFstring", but cJSON gives a number there
// (valuestring == NULL), so every event was silently dropped — the device never
// saw a call answer, so it never sent 200 OK / started media / dropped the leg.
enum ThreeCxEventType {
	TCX_EV_UPSET       = 0,
	TCX_EV_REMOVE      = 1,
	TCX_EV_DTMF        = 2,
	TCX_EV_PROMPT_DONE = 3,
};

// Fallback token lifetime when the JWT can't be decoded: 50 minutes. This is
// deliberately the JWT's real ~1h validity minus margin, NOT the OAuth
// expires_in (3CX reports 60s there, which is wrong and would cause a refresh
// storm that kills the active media streams).
static constexpr int64_t kTokenFallbackLifetimeUs = 50LL * 60 * 1000000;

// Decode a JWT's declared lifetime (exp - iat) in microseconds from its payload
// segment. Returns kTokenFallbackLifetimeUs if anything about the token is
// unparseable. Uses the JWT's own claims so no wall-clock/SNTP is required —
// the result is compared against the monotonic esp_timer.
static int64_t decodeJwtLifetimeUs(const std::string& jwt)
{
	size_t firstDot = jwt.find('.');
	if (firstDot == std::string::npos) return kTokenFallbackLifetimeUs;
	size_t secondDot = jwt.find('.', firstDot + 1);
	if (secondDot == std::string::npos) return kTokenFallbackLifetimeUs;

	std::string payload = jwt.substr(firstDot + 1, secondDot - firstDot - 1);
	if (payload.empty()) return kTokenFallbackLifetimeUs;

	// base64url -> base64, then pad to a multiple of 4.
	for (char& c : payload)
	{
		if (c == '-') c = '+';
		else if (c == '_') c = '/';
	}
	while (payload.size() % 4 != 0) payload.push_back('=');

	std::vector<unsigned char> decoded(payload.size()); // decoded is always smaller
	size_t outLen = 0;
	int rc = mbedtls_base64_decode(decoded.data(), decoded.size(), &outLen,
	                               reinterpret_cast<const unsigned char*>(payload.data()),
	                               payload.size());
	if (rc != 0 || outLen == 0) return kTokenFallbackLifetimeUs;

	std::string json(reinterpret_cast<char*>(decoded.data()), outLen);
	cJSON* root = cJSON_Parse(json.c_str());
	if (!root) return kTokenFallbackLifetimeUs;

	int64_t lifetimeUs = kTokenFallbackLifetimeUs;
	cJSON* exp = cJSON_GetObjectItem(root, "exp");
	cJSON* iat = cJSON_GetObjectItem(root, "iat");
	if (cJSON_IsNumber(exp) && cJSON_IsNumber(iat))
	{
		double span = exp->valuedouble - iat->valuedouble; // seconds
		if (span > 0 && span < 86400) // sanity: positive, under a day
		{
			lifetimeUs = static_cast<int64_t>(span * 1000000.0);
		}
	}
	cJSON_Delete(root);
	return lifetimeUs;
}

ThreeCxAnchorClient::ThreeCxAnchorClient() = default;

ThreeCxAnchorClient::~ThreeCxAnchorClient()
{
	stop();
	if (_rxDoneSem)
	{
		vSemaphoreDelete(_rxDoneSem);
		_rxDoneSem = nullptr;
	}
}

bool ThreeCxAnchorClient::init(const std::string& baseUrl,
                               const std::string& clientId,
                               const std::string& clientSecret,
                               const std::string& sourceDn)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_baseUrl      = baseUrl;
	_clientId     = clientId;
	_clientSecret = clientSecret;
	_sourceDn     = sourceDn;
	return true;
}

bool ThreeCxAnchorClient::start()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_running)
		{
			return false;
		}
		_running = true;
	}

	// 1. Fetch OAuth token
	if (!fetchToken())
	{
		ESP_LOGE(TAG, "Failed to retrieve OAuth token");
		std::lock_guard<std::mutex> lock(_mutex);
		_running = false;
		return false;
	}

	// 2. Connect to control WebSocket
	if (!connectWs())
	{
		ESP_LOGE(TAG, "Failed to connect control WebSocket");
		std::lock_guard<std::mutex> lock(_mutex);
		_running = false;
		return false;
	}

	return true;
}

void ThreeCxAnchorClient::stop()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (!_running)
		{
			return;
		}
		_running   = false;
		_connected = false;
	}

	stopMediaStreams();

	if (_wsClient)
	{
		esp_websocket_client_stop(_wsClient);
		esp_websocket_client_destroy(_wsClient);
		_wsClient = nullptr;
	}

	std::lock_guard<std::mutex> lock(_mutex);
	_eventCb = nullptr;
	_audioCb = nullptr;
}

bool ThreeCxAnchorClient::isConnected() const
{
	return _connected.load(std::memory_order_acquire);
}

bool ThreeCxAnchorClient::isStreaming() const
{
	std::lock_guard<std::mutex> lock(_postMutex);
	return _postClient != nullptr;
}

bool ThreeCxAnchorClient::makeCall(const std::string& destination)
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (!_connected.load(std::memory_order_acquire))
		{
			ESP_LOGW(TAG, "Cannot make call: not connected to 3CX");
			return false;
		}
	}

	// Refresh the OAuth token if it's near expiry. Safe here: no media streams are
	// open at call-origination time, so a re-issue can't kill a live stream.
	ensureToken();

	std::string baseUrl, sourceDn, token;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		baseUrl = _baseUrl;
		sourceDn = _sourceDn;
		token = _accessToken;
	}

	// Trigger outbound call via HTTP POST /callcontrol/{sourceDn}/makecall
	std::string makeCallUrl = baseUrl + "/callcontrol/" + sourceDn + "/makecall";
	esp_http_client_handle_t client = makeAuthedClient(makeCallUrl, HTTP_METHOD_POST, 1024, token);
	if (!client)
	{
		ESP_LOGE(TAG, "Failed to init HTTP client for makeCall");
		return false;
	}

	esp_http_client_set_header(client, "Content-Type", "application/json");

	std::string postData = "{\"destination\":\"" + destination + "\"}";
	esp_http_client_set_post_field(client, postData.c_str(), postData.length());

	int status = 0;
	bool success = performAuthedRequest(client, &status);
	if (success)
	{
		ESP_LOGI(TAG, "Successfully initiated call to %s", destination.c_str());
	}
	else
	{
		ESP_LOGE(TAG, "makeCall request failed (status=%d)", status);
	}

	esp_http_client_cleanup(client);
	return success;
}

bool ThreeCxAnchorClient::dropCall(const std::string& participantId)
{
	std::string partId;
	std::string baseUrl, sourceDn, token;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (!_connected.load(std::memory_order_acquire))
		{
			return false;
		}
		partId = participantId.empty() ? _activeParticipantId : participantId;
		baseUrl = _baseUrl;
		sourceDn = _sourceDn;
		token = _accessToken;
	}

	if (partId.empty())
	{
		return false;
	}

	// Trigger drop via HTTP POST /callcontrol/{sourceDn}/participants/{participantId}/drop
	std::string dropUrl = baseUrl + "/callcontrol/" + sourceDn + "/participants/" + partId + "/drop";
	esp_http_client_handle_t client = makeAuthedClient(dropUrl, HTTP_METHOD_POST, 1024, token);
	if (!client)
	{
		return false;
	}

	int status = 0;
	bool success = performAuthedRequest(client, &status);
	if (success)
	{
		ESP_LOGI(TAG, "Successfully dropped participant %s", partId.c_str());
	}
	else
	{
		ESP_LOGE(TAG, "dropCall request failed for participant %s (status=%d)", partId.c_str(), status);
	}

	esp_http_client_cleanup(client);
	return success;
}

void ThreeCxAnchorClient::setEventCallback(EventCallback cb)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_eventCb = cb;
}

bool ThreeCxAnchorClient::writeAudio(const int16_t* pcmSamples, size_t count)
{
	if (pcmSamples == nullptr || count == 0)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(_postMutex);
	if (!_postClient)
	{
		return false;
	}

	// esp_http_client_open(-1) sets the Transfer-Encoding: chunked header, but
	// esp_http_client_write() is a RAW passthrough to esp_transport_write() — IDF
	// does NOT add the chunk framing it promised (reads are auto-de-chunked,
	// writes are not; verified in esp_http_client.c:1887). So we frame each write
	// ourselves per RFC 9112 §7.1:   <size-hex>\r\n <payload> \r\n
	// Without this, 3CX's HTTP parser reads the first PCM bytes as a chunk-size
	// line, gets garbage, and silently discards the stream — the far end hears
	// nothing while every write reports success. One buffer + one write keeps a
	// whole chunk per TCP segment (helps the spec's "minimize jitter" note). The
	// static buffer is safe: all writers are serialized by _postMutex.
	size_t rawLen = count * sizeof(int16_t);
	static char chunkBuf[8 + 1024 + 2]; // "%X\r\n" header + payload (<=1024) + CRLF
	if (rawLen > 1024)
	{
		return false; // larger than any real audio frame; refuse rather than split
	}

	int hdrLen = snprintf(chunkBuf, sizeof(chunkBuf), "%X\r\n", static_cast<unsigned>(rawLen));
	std::memcpy(chunkBuf + hdrLen, pcmSamples, rawLen);
	chunkBuf[hdrLen + rawLen]     = '\r';
	chunkBuf[hdrLen + rawLen + 1] = '\n';
	const int total = hdrLen + static_cast<int>(rawLen) + 2;

	int written = esp_http_client_write(_postClient, chunkBuf, total);

	// DIAGNOSTIC: cadence + peak amplitude (keep until two-way audio is confirmed).
	static int s_txFrames = 0;
	int peak = 0;
	for (size_t i = 0; i < count; ++i)
	{
		int v = pcmSamples[i] < 0 ? -pcmSamples[i] : pcmSamples[i];
		if (v > peak) peak = v;
	}
	if (written != total)
	{
		ESP_LOGW(TAG, "POST writeAudio short/failed write rc=%d (want %d)", written, total);
	}
	else if (s_txFrames == 0 || (s_txFrames % 50) == 0)
	{
		ESP_LOGI(TAG, "POST writeAudio: frame %d, %u pcm bytes (chunk-framed), peak=%d -> 3CX",
		         s_txFrames, static_cast<unsigned>(rawLen), peak);
	}
	s_txFrames++;
	return (written == total);
}

void ThreeCxAnchorClient::registerAudioRxCallback(AudioRxCallback cb)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_audioCb = cb;
}

// ── Private Helper Functions ───────────────────────────────────────────────

bool ThreeCxAnchorClient::fetchToken()
{
	std::string tokenUrl;
	std::string clientId, clientSecret;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		tokenUrl = _baseUrl + "/connect/token";
		clientId = _clientId;
		clientSecret = _clientSecret;
	}

	esp_http_client_handle_t client = makeAuthedClient(tokenUrl, HTTP_METHOD_POST, 1024);
	if (!client)
	{
		ESP_LOGE(TAG, "Failed to init HTTP client for fetchToken");
		return false;
	}

	esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");

	std::string body = "grant_type=client_credentials&client_id=" + clientId +
	                   "&client_secret=" + clientSecret;

	bool success = false;
	esp_err_t err = esp_http_client_open(client, body.length());
	if (err == ESP_OK)
	{
		int writeBytes = esp_http_client_write(client, body.c_str(), body.length());
		if (writeBytes >= 0)
		{
			int fetch_res = esp_http_client_fetch_headers(client);
			int status = esp_http_client_get_status_code(client);
			ESP_LOGI(TAG, "Token request HTTP status: %d, fetch_res: %d, chunked: %d", 
			         status, fetch_res, esp_http_client_is_chunked_response(client));
			if (status == 200)
			{
				std::string tokenStr;
				if (readJsonStringField(client, "access_token", tokenStr))
				{
					std::lock_guard<std::mutex> lock(_mutex);
					_accessToken = tokenStr;
					_tokenObtainedUs = esp_timer_get_time();
					_tokenLifetimeUs = decodeJwtLifetimeUs(_accessToken);
					if (_wsClient)
					{
						std::string wsHeaders = "Authorization: Bearer " + _accessToken + "\r\n";
						esp_websocket_client_set_headers(_wsClient, wsHeaders.c_str());
					}
					ESP_LOGI(TAG, "Retrieved access token (len=%d, lifetime=%llds)",
					         (int)_accessToken.length(),
					         (long long)(_tokenLifetimeUs / 1000000));
					success = true;
				}
			}
			else
			{
				ESP_LOGE(TAG, "Token request returned HTTP %d", status);
			}
		}
		else
		{
			ESP_LOGE(TAG, "Token failed to write body: %d", writeBytes);
		}
		esp_http_client_close(client);
	}
	else
	{
		ESP_LOGE(TAG, "Token HTTP connection failed to open: %s", esp_err_to_name(err));
	}

	esp_http_client_cleanup(client);
	return success;
}

bool ThreeCxAnchorClient::tokenExpiringSoon() const
{
	if (_tokenObtainedUs == 0 || _tokenLifetimeUs == 0) return true; // no token yet
	// Refresh once we're within 5 minutes of the JWT's declared expiry.
	constexpr int64_t kRefreshMarginUs = 5LL * 60 * 1000000;
	int64_t age = esp_timer_get_time() - _tokenObtainedUs;
	return age >= (_tokenLifetimeUs - kRefreshMarginUs);
}

bool ThreeCxAnchorClient::ensureToken()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (!tokenExpiringSoon()) return true;

		// Never re-issue a token while media streams are live: 3CX invalidates the
		// previous token the instant a new one is granted, which would tear down the
		// chunked GET/POST streams holding the old token mid-call. Refresh only
		// happens between calls (makeCall is invoked before any stream is opened).
		if (_postClient != nullptr || _getClient != nullptr)
		{
			ESP_LOGW(TAG, "Token near expiry but media streams active — deferring refresh");
			return true;
		}
	}

	ESP_LOGI(TAG, "Access token near expiry — refreshing");
	return fetchToken();
}

bool ThreeCxAnchorClient::connectWs()
{
	// Convert base https:// URL to wss:// for call control websocket
	std::string wsUrl;
	std::string token;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		wsUrl = _baseUrl;
		token = _accessToken;
	}

	if (wsUrl.rfind("https://", 0) == 0)
	{
		wsUrl = "wss://" + wsUrl.substr(8) + "/callcontrol/ws";
	}
	else if (wsUrl.rfind("http://", 0) == 0)
	{
		wsUrl = "ws://" + wsUrl.substr(7) + "/callcontrol/ws";
	}
	else
	{
		wsUrl = "wss://" + wsUrl + "/callcontrol/ws";
	}

	// The Bearer token MUST be supplied via wsCfg.headers (the config path), which
	// is what feeds the handshake request. esp_websocket_client_set_headers() does
	// NOT work for handshake auth — it early-returns ESP_ERR_INVALID_ARG unless the
	// client is already CONNECTED, so calling it before start() silently set no
	// header and 3CX rejected the unauthenticated upgrade with HTTP 401. Each header
	// line must be CRLF-terminated. init() strdup's this string, so the local is safe.
	std::string authHeader = "Authorization: Bearer " + token + "\r\n";

	esp_websocket_client_config_t wsCfg = {};
	wsCfg.uri = wsUrl.c_str();
	wsCfg.headers = authHeader.c_str();
	wsCfg.crt_bundle_attach = esp_crt_bundle_attach;
	wsCfg.task_stack = 16384; // needs headroom for two blocking HTTP sessions fired from event handler
	wsCfg.buffer_size = 4096;

	esp_websocket_client_handle_t wsClient = esp_websocket_client_init(&wsCfg);
	if (!wsClient)
	{
		return false;
	}

	esp_err_t err = esp_websocket_register_events(wsClient, WEBSOCKET_EVENT_ANY,
	                                               &ThreeCxAnchorClient::wsEventTrampoline, this);
	if (err != ESP_OK)
	{
		esp_websocket_client_destroy(wsClient);
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(_mutex);
		_wsClient = wsClient;
	}

	err = esp_websocket_client_start(wsClient);
	if (err != ESP_OK)
	{
		esp_websocket_client_destroy(wsClient);
		std::lock_guard<std::mutex> lock(_mutex);
		_wsClient = nullptr;
		return false;
	}

	return true;
}

std::string ThreeCxAnchorClient::getParticipantStatus(const std::string& participantId)
{
	std::string baseUrl, sourceDn, token;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		baseUrl = _baseUrl;
		sourceDn = _sourceDn;
		token = _accessToken;
	}

	std::string getUrl = baseUrl + "/callcontrol/" + sourceDn + "/participants/" + participantId;
	esp_http_client_handle_t client = makeAuthedClient(getUrl, HTTP_METHOD_GET, 1024, token);
	if (!client)
	{
		ESP_LOGE(TAG, "Failed to init HTTP client for getParticipantStatus");
		return "";
	}

	std::string statusStr = "";
	esp_err_t err = esp_http_client_open(client, 0);
	if (err == ESP_OK)
	{
		int fetch_res = esp_http_client_fetch_headers(client);
		int status = esp_http_client_get_status_code(client);
		ESP_LOGI(TAG, "getParticipantStatus HTTP status: %d, fetch_res: %d, chunked: %d",
		         status, fetch_res, esp_http_client_is_chunked_response(client));
		if (status == 200)
		{
			readJsonStringField(client, "status", statusStr);
		}
		else
		{
			ESP_LOGE(TAG, "getParticipantStatus returned HTTP status %d", status);
		}
		esp_http_client_close(client);
	}
	else
	{
		ESP_LOGE(TAG, "getParticipantStatus failed to open connection: %s", esp_err_to_name(err));
	}

	esp_http_client_cleanup(client);
	return statusStr;
}

esp_http_client_handle_t ThreeCxAnchorClient::makeAuthedClient(const std::string& url, esp_http_client_method_t method, int txBufSize, const std::string& token)
{
	esp_http_client_config_t config = {};
	config.url = url.c_str();
	config.method = method;
	config.crt_bundle_attach = esp_crt_bundle_attach;
	config.buffer_size = 4096;
	config.buffer_size_tx = txBufSize;

	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (!client)
	{
		return nullptr;
	}

	if (!token.empty())
	{
		std::string authHeader = "Bearer " + token;
		esp_http_client_set_header(client, "Authorization", authHeader.c_str());
	}

	return client;
}

bool ThreeCxAnchorClient::performAuthedRequest(esp_http_client_handle_t client, int* statusCodeOut)
{
	esp_err_t err = esp_http_client_perform(client);
	int status = -1;
	if (err == ESP_OK)
	{
		status = esp_http_client_get_status_code(client);
		if (statusCodeOut)
		{
			*statusCodeOut = status;
		}
		return (status >= 200 && status < 300);
	}
	else
	{
		ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
		if (statusCodeOut)
		{
			*statusCodeOut = -1;
		}
		return false;
	}
}

bool ThreeCxAnchorClient::readJsonStringField(esp_http_client_handle_t client, const std::string& field, std::string& out)
{
	std::vector<char> buffer;
	char tempBuf[512];
	int readBytes = 0;
	while ((readBytes = esp_http_client_read(client, tempBuf, sizeof(tempBuf))) > 0)
	{
		buffer.insert(buffer.end(), tempBuf, tempBuf + readBytes);
	}
	if (readBytes < 0)
	{
		ESP_LOGE(TAG, "HTTP read error: %d", readBytes);
		return false;
	}
	if (buffer.empty())
	{
		return false;
	}
	buffer.push_back('\0');
	cJSON* root = cJSON_Parse(buffer.data());
	if (!root)
	{
		ESP_LOGE(TAG, "Failed to parse JSON response");
		return false;
	}
	bool found = false;
	cJSON* item = cJSON_GetObjectItem(root, field.c_str());
	if (item && item->valuestring)
	{
		out = item->valuestring;
		found = true;
	}
	cJSON_Delete(root);
	return found;
}

void ThreeCxAnchorClient::wsEventTrampoline(void* handlerArgs, esp_event_base_t /*base*/,
                                           int32_t eventId, void* eventData)
{
	auto* self = static_cast<ThreeCxAnchorClient*>(handlerArgs);
	self->handleWsEvent(eventId, eventData);
}

void ThreeCxAnchorClient::handleWsEvent(int32_t eventId, void* eventData)
{
	auto* data = static_cast<esp_websocket_event_data_t*>(eventData);

	switch (eventId)
	{
		case WEBSOCKET_EVENT_CONNECTED:
			ESP_LOGI(TAG, "Control channel: Websocket connected");
			_connected.store(true, std::memory_order_release);
			break;

		case WEBSOCKET_EVENT_DISCONNECTED:
			ESP_LOGW(TAG, "Control channel: Websocket disconnected");
			_connected.store(false, std::memory_order_release);
			stopMediaStreams();
			break;

		case WEBSOCKET_EVENT_DATA:
			// DIAGNOSTIC: dump every WS data frame raw, before any filtering, so we
			// can see exactly what 3CX pushes on ring/answer/hangup. op_code 0x01=text,
			// 0x02=binary, 0x08=close, 0x09=ping, 0x0A=pong, 0x00=continuation.
			ESP_LOGI(TAG, "WS frame: op=0x%02x len=%d off=%d total=%d", data->op_code,
			         data->data_len, data->payload_offset, data->payload_len);
			if (data->op_code == 0x01 && data->data_ptr != nullptr && data->data_len > 0)
			{
				std::string rawDump(data->data_ptr, data->data_len);
				ESP_LOGI(TAG, "WS payload: %s", rawDump.c_str());
			}

			if (data->op_code == 0x01 && data->data_ptr != nullptr && data->data_len > 0)
			{
				// Received text data from WSS
				std::string payload(data->data_ptr, data->data_len);
				cJSON* root = cJSON_Parse(payload.c_str());
				if (!root) break;
				struct CJsonDeleter {
					cJSON* r;
					~CJsonDeleter() { if (r) cJSON_Delete(r); }
				} deleter{root};

				cJSON* eventObj = cJSON_GetObjectItem(root, "event");
				if (eventObj)
				{
					cJSON* evType = cJSON_GetObjectItem(eventObj, "event_type");
					cJSON* entity = cJSON_GetObjectItem(eventObj, "entity");

					if (cJSON_IsNumber(evType) && entity && entity->valuestring)
					{
						int evTypeNum = evType->valueint;
						std::string entityStr = entity->valuestring;

						// Parse entity path to verify DN and participantId:
						// /callcontrol/{dn}/participants/{id}
						std::vector<std::string> pathTokens;
						std::stringstream ss(entityStr);
						std::string item;
						while (std::getline(ss, item, '/'))
						{
							if (!item.empty()) pathTokens.push_back(item);
						}

						// Expect: ["callcontrol", dn, "participants", id] -> size == 4
						if (pathTokens.size() == 4 && pathTokens[0] == "callcontrol" && pathTokens[2] == "participants")
						{
							std::string dn = pathTokens[1];
							std::string partId = pathTokens[3];

							if (dn == _sourceDn)
							{
								EventCallback evCb;
								{
									std::lock_guard<std::mutex> lock(_mutex);
									evCb = _eventCb;
								}

								if (evTypeNum == TCX_EV_UPSET)
								{
									// 3CX repeats Upset for a participant every ~750ms. Once we
									// are already streaming this participant, skip the work:
									// getParticipantStatus is a blocking TLS GET on this WS task,
									// and re-firing Answered would re-send 200 OK to the handset.
									bool isSameActivePart = false;
									{
										std::lock_guard<std::mutex> lock(_mutex);
										isSameActivePart = (_activeParticipantId == partId);
									}
									if (isSameActivePart)
									{
										break;
									}

									ESP_LOGI(TAG, "Call control event: Participant Upset %s", partId.c_str());
									std::string statusStr = getParticipantStatus(partId);
									ESP_LOGI(TAG, "Participant %s status: %s", partId.c_str(), statusStr.c_str());

									if (statusStr == "Connected")
									{
										// Set _activeParticipantId BEFORE the callback so any
										// Upset that races in behind us takes the skip path above.
										if (startMediaStreams(partId))
										{
											if (evCb)
											{
												CallEvent ev{CallEvent::Answered, partId, ""};
												evCb(ev);
											}
										}
										else
										{
											ESP_LOGE(TAG, "Failed to start media streams for participant %s", partId.c_str());
											stopMediaStreams();
										}
									}
								}
								else if (evTypeNum == TCX_EV_REMOVE)
								{
									ESP_LOGI(TAG, "Call control event: Participant Remove %s", partId.c_str());
									bool isSameActivePart = false;
									{
										std::lock_guard<std::mutex> lock(_mutex);
										isSameActivePart = (_activeParticipantId == partId);
									}
									if (isSameActivePart)
									{
										stopMediaStreams();
									}

									if (evCb)
									{
										CallEvent ev{CallEvent::Dropped, partId, ""};
										evCb(ev);
									}
								}
								else if (evTypeNum == TCX_EV_DTMF)
								{
									cJSON* attached = cJSON_GetObjectItem(eventObj, "attached_data");
									if (attached)
									{
										cJSON* dtmfInput = cJSON_GetObjectItem(attached, "dtmf_input");
										if (dtmfInput && dtmfInput->valuestring && evCb)
										{
											CallEvent ev{CallEvent::Dtmf, partId, dtmfInput->valuestring};
											evCb(ev);
										}
									}
								}
							}
						}
					}
				}
			}
			break;

		default:
			break;
	}
}

bool ThreeCxAnchorClient::startMediaStreams(const std::string& participantId)
{
	ESP_LOGI(TAG, "Starting media streams for participant %s", participantId.c_str());

	// Single-call guard
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_rxTaskHandle != nullptr)
		{
			ESP_LOGW(TAG, "startMediaStreams: Media streams already active/starting");
			return false;
		}
	}

	std::string baseUrl, sourceDn, token;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_activeParticipantId = participantId;
		baseUrl = _baseUrl;
		sourceDn = _sourceDn;
		token = _accessToken;
	}

	// 1. Initialize HTTP POST client for outbound audio
	std::string postUrl = baseUrl + "/callcontrol/" + sourceDn + "/participants/" + participantId + "/stream";
	
	esp_http_client_handle_t postClient = nullptr;
	{
		std::lock_guard<std::mutex> postLock(_postMutex);
		_postClient = makeAuthedClient(postUrl, HTTP_METHOD_POST, 4096, token);
		if (!_postClient)
		{
			ESP_LOGE(TAG, "Failed to init POST HTTP client");
			return false;
		}
		postClient = _postClient;

		esp_http_client_set_header(postClient, "Content-Type", "application/octet-stream");

		// write_len = -1 puts esp_http_client into chunked transfer-encoding mode (it
		// adds the Transfer-Encoding header and frames each esp_http_client_write as a
		// chunk). Passing 0 here was the outbound-audio bug: it sent Content-Length: 0,
		// so 3CX saw an empty body and closed the stream and every writeAudio() wrote
		// into a dead socket. Do NOT set Transfer-Encoding manually — IDF owns it now.
		esp_err_t err = esp_http_client_open(postClient, -1); // -1 => chunked
		if (err != ESP_OK)
		{
			ESP_LOGE(TAG, "Failed to open chunked HTTP POST stream: %s", esp_err_to_name(err));
			esp_http_client_cleanup(postClient);
			_postClient = nullptr;
			return false;
		}
	}
	ESP_LOGI(TAG, "POST (device->3CX) audio stream OPEN: %s", postUrl.c_str());

	// 2. Initialize HTTP GET client and launch Rx task for inbound audio
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_rxDoneSem)
		{
			vSemaphoreDelete(_rxDoneSem);
		}
		_rxDoneSem = xSemaphoreCreateBinary();
		BaseType_t rc = xTaskCreatePinnedToCore(&ThreeCxAnchorClient::rxTaskTrampoline, "3cx_media_rx", 6144, this, 6, &_rxTaskHandle, 1);
		if (rc != pdPASS)
		{
			ESP_LOGE(TAG, "Failed to create Rx task");
			_rxTaskHandle = nullptr;
			{
				std::lock_guard<std::mutex> postLock(_postMutex);
				if (_postClient)
				{
					esp_http_client_close(_postClient);
					esp_http_client_cleanup(_postClient);
					_postClient = nullptr;
				}
			}
			return false;
		}
	}

	return true;
}

void ThreeCxAnchorClient::stopMediaStreams()
{
	TaskHandle_t taskToKill = nullptr;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_activeParticipantId.clear();
		taskToKill = _rxTaskHandle;
		_rxTaskHandle = nullptr; // signal task to exit
	}

	if (taskToKill)
	{
		{
			std::lock_guard<std::mutex> lock(_getMutex);
			if (_getClient)
			{
				int fd = esp_http_client_get_socket(_getClient);
				if (fd >= 0)
				{
					shutdown(fd, SHUT_RDWR);
				}
			}
		}

		if (_rxDoneSem)
		{
			if (xSemaphoreTake(_rxDoneSem, pdMS_TO_TICKS(2000)) != pdTRUE)
			{
				ESP_LOGE(TAG, "Rx task failed to exit in time! Forcing task deletion.");
				vTaskDelete(taskToKill);
				// Force cleanup since task was killed and won't clean up itself.
				// Use try_lock: if the deleted task was holding _getMutex when it
				// was killed, the mutex is permanently poisoned and a blocking
				// lock_guard would deadlock here.
				if (_getMutex.try_lock())
				{
					if (_getClient)
					{
						esp_http_client_close(_getClient);
						esp_http_client_cleanup(_getClient);
						_getClient = nullptr;
					}
					_getMutex.unlock();
				}
				else
				{
					ESP_LOGE(TAG, "_getMutex poisoned by killed task — leaking _getClient to avoid deadlock");
				}
			}
		}
	}
	else
	{
		// No rx task running — safe to clean up directly.
		std::lock_guard<std::mutex> lock(_getMutex);
		if (_getClient)
		{
			esp_http_client_close(_getClient);
			esp_http_client_cleanup(_getClient);
			_getClient = nullptr;
		}
	}

	{
		std::lock_guard<std::mutex> lock(_postMutex);
		if (_postClient)
		{
			// RFC 9112 §7.1 last-chunk: terminate the chunked body cleanly so 3CX
			// sees end-of-stream rather than an aborted socket.
			esp_http_client_write(_postClient, "0\r\n\r\n", 5);
			esp_http_client_close(_postClient);
			esp_http_client_cleanup(_postClient);
			_postClient = nullptr;
		}
	}
	ESP_LOGI(TAG, "Media streams stopped");
}

void ThreeCxAnchorClient::rxTaskTrampoline(void* arg)
{
	auto* self = static_cast<ThreeCxAnchorClient*>(arg);
	self->runRxLoop();
	if (self->_rxDoneSem)
	{
		xSemaphoreGive(self->_rxDoneSem);
	}
	vTaskDelete(nullptr);
}

void ThreeCxAnchorClient::runRxLoop()
{
	std::string activePartId;
	std::string baseUrl, sourceDn;
	std::string token;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		activePartId = _activeParticipantId;
		baseUrl = _baseUrl;
		sourceDn = _sourceDn;
		token = _accessToken;
	}

	std::string getUrl = baseUrl + "/callcontrol/" + sourceDn + "/participants/" + activePartId + "/stream";

	// _getClient lifecycle (init/close/cleanup) is serialized with
	// stopMediaStreams() via _getMutex so the unblock-close and our teardown can
	// never double-free the handle. The lock is NEVER held across the blocking
	// open/fetch/read calls — stopMediaStreams() must be able to grab it to close
	// the handle and unblock us.
	auto teardownGetClient = [this]() {
		std::lock_guard<std::mutex> lock(_getMutex);
		if (_getClient)
		{
			esp_http_client_close(_getClient);
			esp_http_client_cleanup(_getClient);
			_getClient = nullptr;
		}
	};

	// The /stream endpoint returns 404 (participant not found yet) or 424 (failed
	// dependency) until the call leg is actually connected and media is flowing.
	// startMediaStreams() can fire slightly ahead of that, so retry the open
	// rather than giving up — otherwise inbound audio is lost for the whole call.
	constexpr int        kMaxAttempts = 40;                  // ~20s ceiling
	const TickType_t     kRetryDelay  = pdMS_TO_TICKS(500);
	bool opened = false;

	for (int attempt = 0; attempt < kMaxAttempts && _rxTaskHandle != nullptr; ++attempt)
	{
		{
			std::lock_guard<std::mutex> lock(_getMutex);
			if (_rxTaskHandle == nullptr) break; // stop requested mid-retry
			_getClient = makeAuthedClient(getUrl, HTTP_METHOD_GET, 1024, token);
		}
		if (!_getClient)
		{
			vTaskDelay(kRetryDelay);
			continue;
		}

		esp_err_t err = esp_http_client_open(_getClient, 0);
		if (err == ESP_OK)
		{
			esp_http_client_fetch_headers(_getClient);
			int status = esp_http_client_get_status_code(_getClient);
			if (status == 200)
			{
				opened = true;
				break;
			}
			ESP_LOGW(TAG, "GET stream not ready (HTTP %d), attempt %d/%d", status, attempt + 1, kMaxAttempts);
		}
		else
		{
			ESP_LOGW(TAG, "GET stream open failed (%s), attempt %d/%d", esp_err_to_name(err), attempt + 1, kMaxAttempts);
		}

		teardownGetClient();
		if (_rxTaskHandle != nullptr)
		{
			vTaskDelay(kRetryDelay);
		}
	}

	if (!opened)
	{
		ESP_LOGW(TAG, "GET (3CX->device) stream never opened — no inbound audio");
		teardownGetClient();
		return;
	}
	ESP_LOGI(TAG, "GET (3CX->device) audio stream OPEN: %s", getUrl.c_str());

	alignas(2) char readBuf[514]; // +1 for carry byte, +1 for alignment padding; alignas(2) for safe int16_t reinterpret_cast
	bool hasCarry = false;
	char carryByte = 0;
	int rxReads = 0;
	while (_rxTaskHandle != nullptr)
	{
		int bytesRead = 0;
		if (hasCarry)
		{
			readBuf[0] = carryByte;
			int res = esp_http_client_read(_getClient, readBuf + 1, sizeof(readBuf) - 1);
			if (res <= 0)
			{
				break;
			}
			bytesRead = 1 + res;
			hasCarry = false;
		}
		else
		{
			int res = esp_http_client_read(_getClient, readBuf, sizeof(readBuf) - 1);
			if (res <= 0)
			{
				break;
			}
			bytesRead = res;
		}

		if (bytesRead % 2 != 0)
		{
			carryByte = readBuf[bytesRead - 1];
			hasCarry = true;
			bytesRead -= 1;
		}

		if (bytesRead == 0)
		{
			continue;
		}

		// DIAGNOSTIC: confirm 3CX->device audio is arriving. First read + every ~100th.
		if (rxReads == 0 || (rxReads % 100) == 0)
		{
			ESP_LOGI(TAG, "GET read: chunk %d, %d bytes <- 3CX", rxReads, bytesRead);
		}
		rxReads++;

		AudioRxCallback audioCb;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			audioCb = _audioCb;
		}

		if (audioCb)
		{
			// Process raw bytes into linear PCM16 samples (each sample is 2 bytes, little-endian)
			size_t sampleCount = bytesRead / sizeof(int16_t);
			const int16_t* pcmSamples = reinterpret_cast<const int16_t*>(readBuf);
			if (sampleCount > 0)
			{
				audioCb(pcmSamples, sampleCount);
			}
		}
	}

	teardownGetClient();
}

#endif
