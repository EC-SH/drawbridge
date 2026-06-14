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

bool ThreeCxAnchorClient::answerCall(const std::string&)
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

void ThreeCxAnchorClient::tick()
{
}

#else

// ── ESP-IDF Implementation: real mTLS, WSS and chunked HTTPS streams ─────────
#include "esp_log.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "UrlEncode.hpp"
#include <sstream>
#include <cstring>
#include <cstdio>
#include <vector>
#include <cstdint>
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

static const char* TAG = "ThreeCxAnchor";

// RAII guard: cJSON_Delete on scope exit. Shared by the WS event parser and the
// live-state reconcile/device GET helpers so every parse path frees on every return.
namespace {
struct CJsonDeleter
{
	cJSON* r;
	~CJsonDeleter() { if (r) cJSON_Delete(r); }
};
} // namespace

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

	// 3. Pre-establish the control-plane TLS session so the first makecall
	// doesn't pay the handshake during post-dial delay.
	warmCtrlConnection();

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
	closeCtrlClient();

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

	std::string baseUrl, sourceDn, deviceId;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		baseUrl = _baseUrl;
		sourceDn = _sourceDn;
		deviceId = _deviceId;
	}

	// Prefer the device-specific makecall endpoint (the recommended 3CX transport) over the legacy
	// /callcontrol/{dn}/makecall. Resolve the device_id lazily — makeCall runs on the 3cx_makecall
	// worker, so the blocking GET is fine here.
	if (deviceId.empty() && resolveDevice())
	{
		std::lock_guard<std::mutex> lock(_mutex);
		deviceId = _deviceId;
	}

	const std::string legacyUrl = baseUrl + "/callcontrol/" + sourceDn + "/makecall";
	auto deviceUrl = [&](const std::string& id) {
		return baseUrl + "/callcontrol/" + sourceDn + "/devices/" + urlEncode(id) + "/makecall";
	};
	std::string makeCallUrl = deviceId.empty() ? legacyUrl : deviceUrl(deviceId);
	const std::string postData = "{\"destination\":\"" + destination + "\"}";

	// Mark an outbound call in flight BEFORE the POST so the participant upserts 3CX pushes for it
	// are classified as ours (not mistaken for an inbound PSTN call). Stamp the set-time in the
	// same _mutex critical section so the tick() watchdog can later detect a makecall that 3CX
	// accepted (2xx) but that never produced a participant.
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_outboundActiveSetUs = esp_timer_get_time();
		_outboundActive.store(true, std::memory_order_release);
	}

	// Capture the makecall RESPONSE BODY (not just the status) so we can read result.id — the
	// initiator's OWN leg on our DN, which is the leg 3CX authorizes us to stream/drop. (httpPostBody
	// uses a fresh client; performCtrl's persistent handle does not expose the body.)
	int status = 0;
	std::string respBody;
	bool success = httpPostBody(makeCallUrl, "application/json", postData, respBody, &status);

	// A device-path 404 means the cached device_id went stale (registration flap). Re-resolve once
	// on a fresh id and retry; if that still fails, fall back to the legacy endpoint.
	if (!success && !deviceId.empty() && status == 404)
	{
		ESP_LOGW(TAG, "makeCall: device path 404 — re-resolving device_id");
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_deviceId.clear();
		}
		std::string freshId;
		if (resolveDevice())
		{
			std::lock_guard<std::mutex> lock(_mutex);
			freshId = _deviceId;
		}
		if (!freshId.empty())
		{
			status = 0; respBody.clear();
			success = httpPostBody(deviceUrl(freshId), "application/json", postData, respBody, &status);
		}
		if (!success)
		{
			ESP_LOGW(TAG, "makeCall: falling back to legacy makecall endpoint");
			status = 0; respBody.clear();
			success = httpPostBody(legacyUrl, "application/json", postData, respBody, &status);
		}
	}

	if (success)
	{
		// Select the leg WE control: makecall result.id, else a destination digit-suffix match in
		// the live participant list (lnp-audit's _makecall id resolution). We deliberately do NOT
		// adopt the participant id 3CX later surfaces over the WS — that can be the far leg, on which
		// a specific-id GET/drop returns 403 (issue #40). Drop/media key off this owned id.
		std::string ownLeg = resolveOutboundLeg(respBody, destination);
		if (!ownLeg.empty())
		{
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_activeParticipantId = ownLeg;
			}
			startRxIfNeeded(ownLeg);   // prime the GET-stream retry loop on OUR leg
			ESP_LOGI(TAG, "Successfully initiated call to %s (own leg %s)", destination.c_str(), ownLeg.c_str());
		}
		else
		{
			ESP_LOGW(TAG, "makeCall: initiated %s but could not resolve own leg id — teardown will rely on reconcile", destination.c_str());
		}
	}
	else
	{
		ESP_LOGE(TAG, "makeCall request failed (status=%d)", status);
		_outboundActive.store(false, std::memory_order_release);   // POST failed → not in flight
	}
	return success;
}

bool ThreeCxAnchorClient::dropCall(const std::string& participantId)
{
	std::string partId;
	std::string baseUrl, sourceDn;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (!_connected.load(std::memory_order_acquire))
		{
			return false;
		}
		partId = participantId.empty() ? _activeParticipantId : participantId;
		baseUrl = _baseUrl;
		sourceDn = _sourceDn;
	}

	// No id from the dialog arg or the WS upset cache (_activeParticipantId) — the
	// registration-dependent miss where no upset ever correlated. Ask 3CX which participant is
	// actually live on the DN so the leg still gets torn down. Runs on the 3cx_dropcall worker,
	// so this blocking GET is off the SIP/WS threads.
	if (partId.empty())
	{
		partId = reconcileParticipantId();
		if (!partId.empty())
		{
			ESP_LOGI(TAG, "dropCall: reconciled live participant %s", partId.c_str());
		}
	}

	if (partId.empty())
	{
		return false;
	}

	// Trigger drop via HTTP POST /callcontrol/{sourceDn}/participants/{participantId}/drop
	// on the persistent control connection — fast teardown, no fresh handshake.
	std::string dropUrl = baseUrl + "/callcontrol/" + sourceDn + "/participants/" + partId + "/drop";

	int status = 0;
	// The 3CX participant-action endpoint requires an application/json body — the
	// reference client/server posts "{}" with Content-Type application/json (see the
	// call-control-examples server controlParticipant()). An empty body with no
	// Content-Type (the old call) is rejected, so the drop silently never fired and
	// the PSTN leg lingered until 3CX's own timeout. makeCall already does this right.
	bool success = performCtrl(dropUrl, "application/json", "{}", &status);
	if (success)
	{
		ESP_LOGI(TAG, "Successfully dropped participant %s", partId.c_str());
	}
	else
	{
		ESP_LOGE(TAG, "dropCall request failed for participant %s (status=%d)", partId.c_str(), status);
	}
	return success;
}

bool ThreeCxAnchorClient::answerCall(const std::string& participantId)
{
	std::string partId, baseUrl, sourceDn;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (!_connected.load(std::memory_order_acquire))
		{
			return false;
		}
		partId = participantId.empty() ? _activeParticipantId : participantId;
		baseUrl = _baseUrl;
		sourceDn = _sourceDn;
	}
	if (partId.empty())
	{
		return false;
	}

	// Answer the inbound participant the route point is offering us: POST
	// /callcontrol/{dn}/participants/{id}/answer on the persistent control connection.
	// 3CX then connects the PSTN leg, flips the participant to Connected, and the
	// existing Upset→Connected path opens the PCM streams (startMediaStreams).
	//
	// NOTE: a route point configured as an *External Call Flow* app may instead expect
	// /route (route the call into the app) rather than /answer. If a deployment's
	// inbound calls never connect, this action string is the first thing to flip.
	std::string answerUrl = baseUrl + "/callcontrol/" + sourceDn + "/participants/" + partId + "/answer";

	int status = 0;
	// Same as drop: the participant-action endpoint wants an application/json "{}"
	// body (call-control-examples controlParticipant()), not an empty body.
	bool success = performCtrl(answerUrl, "application/json", "{}", &status);
	if (success)
	{
		ESP_LOGI(TAG, "Answered inbound participant %s", partId.c_str());
	}
	else
	{
		ESP_LOGE(TAG, "answerCall request failed for participant %s (status=%d)", partId.c_str(), status);
	}
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
	}

	// Never re-issue a token while media streams are live: 3CX invalidates the
	// previous token the instant a new one is granted, which would tear down the
	// chunked GET/POST streams holding the old token mid-call. Refresh only
	// happens between calls (makeCall is invoked before any stream is opened).
	// Each handle is read under the mutex that actually guards its lifecycle
	// (_postMutex / _getMutex) — reading them under _mutex synchronized nothing
	// (Issue #14). Taken sequentially, never nested, so no ordering hazard.
	bool streamsActive = false;
	{
		std::lock_guard<std::mutex> lock(_postMutex);
		streamsActive = (_postClient != nullptr);
	}
	if (!streamsActive)
	{
		std::lock_guard<std::mutex> lock(_getMutex);
		streamsActive = (_getClient != nullptr);
	}
	if (streamsActive)
	{
		ESP_LOGW(TAG, "Token near expiry but media streams active — deferring refresh");
		return true;
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

// Generalized authed GET → full response body. Snapshots the token under _mutex, then does all
// blocking I/O lock-free. close()+cleanup() on every exit path. Returns true only on a 2xx with a
// clean body read; *statusOut carries the HTTP status (or -1) so callers can distinguish "no
// evidence" (transient/non-200) from a definitive answer.
bool ThreeCxAnchorClient::httpGetBody(const std::string& url, std::string& bodyOut, int* statusOut)
{
	if (statusOut) *statusOut = -1;
	bodyOut.clear();

	std::string token;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		token = _accessToken;
	}

	esp_http_client_handle_t client = makeAuthedClient(url, HTTP_METHOD_GET, 1024, token);
	if (!client)
	{
		ESP_LOGE(TAG, "httpGetBody: failed to init HTTP client");
		return false;
	}

	bool ok = false;
	esp_err_t err = esp_http_client_open(client, 0);
	if (err == ESP_OK)
	{
		esp_http_client_fetch_headers(client);
		int status = esp_http_client_get_status_code(client);
		if (statusOut) *statusOut = status;

		// Drain the whole body regardless of status so the connection is left clean.
		std::vector<char> buffer;
		char tempBuf[512];
		int readBytes = 0;
		while ((readBytes = esp_http_client_read(client, tempBuf, sizeof(tempBuf))) > 0)
		{
			buffer.insert(buffer.end(), tempBuf, tempBuf + readBytes);
		}
		if (readBytes < 0)
		{
			ESP_LOGE(TAG, "httpGetBody: HTTP read error %d (status=%d)", readBytes, status);
		}
		else
		{
			bodyOut.assign(buffer.begin(), buffer.end());
			ok = (status >= 200 && status < 300);
		}
		esp_http_client_close(client);
	}
	else
	{
		ESP_LOGE(TAG, "httpGetBody: open failed: %s", esp_err_to_name(err));
	}

	esp_http_client_cleanup(client);
	return ok;
}

// POST with a body and return the response body (and HTTP status). Mirrors httpGetBody but for the
// makecall, whose result.id we need to read. Uses a fresh client (the persistent _ctrlClient that
// performCtrl drives via esp_http_client_perform does not surface the body). close()+cleanup() on
// every path; *statusOut carries the HTTP status (or -1).
bool ThreeCxAnchorClient::httpPostBody(const std::string& url, const char* contentType, const std::string& body, std::string& respBody, int* statusOut)
{
	if (statusOut) *statusOut = -1;
	respBody.clear();

	std::string token;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		token = _accessToken;
	}

	esp_http_client_handle_t client = makeAuthedClient(url, HTTP_METHOD_POST, 1024, token);
	if (!client)
	{
		ESP_LOGE(TAG, "httpPostBody: failed to init HTTP client");
		return false;
	}
	if (contentType)
	{
		esp_http_client_set_header(client, "Content-Type", contentType);
	}

	bool ok = false;
	esp_err_t err = esp_http_client_open(client, static_cast<int>(body.length()));
	if (err == ESP_OK)
	{
		int wlen = (body.empty()) ? 0 : esp_http_client_write(client, body.c_str(), body.length());
		if (wlen >= 0)
		{
			esp_http_client_fetch_headers(client);
			int status = esp_http_client_get_status_code(client);
			if (statusOut) *statusOut = status;

			std::vector<char> buffer;
			char tempBuf[512];
			int readBytes = 0;
			while ((readBytes = esp_http_client_read(client, tempBuf, sizeof(tempBuf))) > 0)
			{
				buffer.insert(buffer.end(), tempBuf, tempBuf + readBytes);
			}
			if (readBytes < 0)
			{
				ESP_LOGE(TAG, "httpPostBody: HTTP read error %d (status=%d)", readBytes, status);
			}
			else
			{
				respBody.assign(buffer.begin(), buffer.end());
				ok = (status >= 200 && status < 300);
			}
		}
		else
		{
			ESP_LOGE(TAG, "httpPostBody: write failed %d", wlen);
		}
		esp_http_client_close(client);
	}
	else
	{
		ESP_LOGE(TAG, "httpPostBody: open failed: %s", esp_err_to_name(err));
	}

	esp_http_client_cleanup(client);
	return ok;
}

// The first participant id currently on our DN, or "" if none / on any error. Drives the drop
// fallback when no WS upset ever correlated an id (dropCall) and the wedge watchdog (tick worker).
std::string ThreeCxAnchorClient::reconcileParticipantId()
{
	std::string url;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		url = _baseUrl + "/callcontrol/" + _sourceDn + "/participants";
	}

	std::string body;
	int status = 0;
	if (!httpGetBody(url, body, &status) || status != 200)
	{
		ESP_LOGW(TAG, "reconcileParticipantId: GET failed (status=%d)", status);
		return "";
	}

	cJSON* root = cJSON_Parse(body.c_str());
	if (!root)
	{
		ESP_LOGE(TAG, "reconcileParticipantId: JSON parse failed");
		return "";
	}
	CJsonDeleter deleter{root};

	if (!cJSON_IsArray(root))
	{
		return "";
	}

	// Single-leg assumption: our DN bridges one call at a time via MediaBridge, so the first
	// participant IS the only one. A multi-leg DN would need a target match instead of "first" —
	// flag this if such a deployment ever appears. A leg ending between this GET and the drop that
	// follows is a benign 404 at the drop POST.
	cJSON* elem = nullptr;
	cJSON_ArrayForEach(elem, root)
	{
		if (!cJSON_IsObject(elem)) continue;
		cJSON* id = cJSON_GetObjectItem(elem, "id");
		if (cJSON_IsString(id) && id->valuestring && id->valuestring[0])
		{
			return std::string(id->valuestring);
		}
		if (cJSON_IsNumber(id))
		{
			char idbuf[24];
			snprintf(idbuf, sizeof(idbuf), "%d", id->valueint);
			return std::string(idbuf);
		}
	}
	return "";
}

// Status of one specific leg, read from the participant LIST (GET /callcontrol/{dn}/participants
// then find id). This is the controllable-scope read 3CX honours, unlike GET .../participants/{id}
// which 403s for a leg this DN does not directly control (issue #40). "" if the leg isn't listed.
std::string ThreeCxAnchorClient::getLegStatus(const std::string& legId)
{
	if (legId.empty()) return "";
	std::string url;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		url = _baseUrl + "/callcontrol/" + _sourceDn + "/participants";
	}
	std::string body;
	int status = 0;
	if (!httpGetBody(url, body, &status) || status != 200)
	{
		ESP_LOGW(TAG, "getLegStatus: GET /participants failed (status=%d)", status);
		return "";
	}
	cJSON* root = cJSON_Parse(body.c_str());
	if (!root) return "";
	CJsonDeleter deleter{root};
	if (!cJSON_IsArray(root)) return "";

	cJSON* elem = nullptr;
	cJSON_ArrayForEach(elem, root)
	{
		if (!cJSON_IsObject(elem)) continue;
		cJSON* id = cJSON_GetObjectItem(elem, "id");
		char idbuf[24] = {0};
		if (cJSON_IsString(id) && id->valuestring) snprintf(idbuf, sizeof(idbuf), "%s", id->valuestring);
		else if (cJSON_IsNumber(id))               snprintf(idbuf, sizeof(idbuf), "%d", id->valueint);
		if (legId == idbuf)
		{
			cJSON* st = cJSON_GetObjectItem(elem, "status");
			if (cJSON_IsString(st) && st->valuestring) return std::string(st->valuestring);
			return "";
		}
	}
	return "";   // leg not on the DN (gone, or an id we don't own)
}

// Resolve the leg WE control for an outbound makecall (lnp-audit's _makecall id logic): prefer the
// response result.id (the initiator's own leg on our DN); failing that (legacy path / absent under
// deregistration), match the just-created leg in the live participant list by destination digit
// suffix. Returns "" if neither yields an id. This is the only id 3CX lets us stream/drop (issue #40).
std::string ThreeCxAnchorClient::resolveOutboundLeg(const std::string& makecallRespBody, const std::string& destination)
{
	// 1) result.id from the makecall response.
	if (!makecallRespBody.empty())
	{
		cJSON* root = cJSON_Parse(makecallRespBody.c_str());
		if (root)
		{
			CJsonDeleter deleter{root};
			cJSON* result = cJSON_GetObjectItem(root, "result");
			if (result)
			{
				cJSON* id = cJSON_GetObjectItem(result, "id");
				if (cJSON_IsString(id) && id->valuestring && id->valuestring[0]) return std::string(id->valuestring);
				if (cJSON_IsNumber(id)) { char b[24]; snprintf(b, sizeof(b), "%d", id->valueint); return std::string(b); }
			}
		}
	}

	// 2) Fallback: destination digit-suffix match against the live list.
	std::string digits;
	for (char c : destination) if (c >= '0' && c <= '9') digits.push_back(c);
	if (digits.empty()) return "";
	const std::string suffix = digits.size() > 7 ? digits.substr(digits.size() - 7) : digits;

	std::string url;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		url = _baseUrl + "/callcontrol/" + _sourceDn + "/participants";
	}
	std::string body;
	int status = 0;
	if (!httpGetBody(url, body, &status) || status != 200) return "";
	cJSON* root = cJSON_Parse(body.c_str());
	if (!root) return "";
	CJsonDeleter deleter{root};
	if (!cJSON_IsArray(root)) return "";

	cJSON* elem = nullptr;
	cJSON_ArrayForEach(elem, root)
	{
		if (!cJSON_IsObject(elem)) continue;
		for (const char* k : { "party_dn", "party_caller_id", "party_did" })
		{
			cJSON* f = cJSON_GetObjectItem(elem, k);
			if (!cJSON_IsString(f) || !f->valuestring) continue;
			std::string fd;
			for (const char* p = f->valuestring; *p; ++p) if (*p >= '0' && *p <= '9') fd.push_back(*p);
			if (fd.size() >= suffix.size() && fd.compare(fd.size() - suffix.size(), suffix.size(), suffix) == 0)
			{
				cJSON* id = cJSON_GetObjectItem(elem, "id");
				if (cJSON_IsString(id) && id->valuestring && id->valuestring[0]) return std::string(id->valuestring);
				if (cJSON_IsNumber(id)) { char b[24]; snprintf(b, sizeof(b), "%d", id->valueint); return std::string(b); }
			}
		}
	}
	return "";
}

// Parse a 3CX /devices array, returning the best device_id: prefer a device that advertises a
// user_agent (a real registered endpoint) over a bare slot; otherwise the first device_id present.
// "" if none / on parse error.
std::string ThreeCxAnchorClient::pickDeviceId(const std::string& body)
{
	cJSON* root = cJSON_Parse(body.c_str());
	if (!root) return "";
	CJsonDeleter deleter{root};
	if (!cJSON_IsArray(root)) return "";

	std::string firstId;
	cJSON* elem = nullptr;
	cJSON_ArrayForEach(elem, root)
	{
		if (!cJSON_IsObject(elem)) continue;
		cJSON* devId = cJSON_GetObjectItem(elem, "device_id");
		if (!cJSON_IsString(devId) || !devId->valuestring || !devId->valuestring[0]) continue;

		if (firstId.empty()) firstId = devId->valuestring;

		cJSON* ua = cJSON_GetObjectItem(elem, "user_agent");
		if (cJSON_IsString(ua) && ua->valuestring && ua->valuestring[0])
		{
			return std::string(devId->valuestring);   // registered endpoint — prefer it
		}
	}
	return firstId;
}

// GET /callcontrol/{dn}/devices → pickDeviceId → cache in _deviceId. Returns true on success.
bool ThreeCxAnchorClient::resolveDevice()
{
	std::string url;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		url = _baseUrl + "/callcontrol/" + _sourceDn + "/devices";
	}

	std::string body;
	int status = 0;
	if (!httpGetBody(url, body, &status) || status != 200)
	{
		ESP_LOGW(TAG, "resolveDevice: GET /devices failed (status=%d)", status);
		return false;
	}

	std::string devId = pickDeviceId(body);
	if (devId.empty())
	{
		ESP_LOGW(TAG, "resolveDevice: no usable device_id on DN");
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(_mutex);
		_deviceId = devId;
	}
	ESP_LOGI(TAG, "resolveDevice: using device_id %s", devId.c_str());
	return true;
}

void ThreeCxAnchorClient::tick()
{
	// Runs inline on the SIP task at ≤1 Hz — MUST be non-blocking: only atomic reads, one timer
	// read, and (rarely) a worker spawn. No logging or allocation on the common path.

	// Issue #65 (L-1): too many leaked GET sockets — spawn a one-shot worker to do a full
	// stop()/start() reclaim OFF this task (stop()/start() block on TLS I/O). _restartInFlight
	// is a one-shot gate so repeated ticks can't stack restart workers. Checked before the
	// outboundActive gate because a leak can accumulate independent of an active outbound call.
	if (_restartRequested.load(std::memory_order_acquire) &&
	    !_restartInFlight.load(std::memory_order_acquire))
	{
		_restartInFlight.store(true, std::memory_order_release);
		if (xTaskCreate(&ThreeCxAnchorClient::restartTaskTrampoline, "3cx_restart", 6144, this, 5, nullptr) != pdPASS)
		{
			ESP_LOGE(TAG, "tick: failed to spawn anchor-restart worker");
			_restartInFlight.store(false, std::memory_order_release);
		}
	}

	if (!_outboundActive.load(std::memory_order_acquire)) return;
	if (_reconcileInFlight.load(std::memory_order_acquire)) return;

	int64_t setUs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		setUs = _outboundActiveSetUs;
	}
	// 15 s grace: a real outbound call yields its participant upset within ~1 RTT, so a flag still
	// set this long after makecall means the participant never materialized — i.e. a wedge.
	constexpr int64_t kWedgeGraceUs = 15LL * 1000000;
	if (esp_timer_get_time() - setUs < kWedgeGraceUs) return;

	// Claim the one-shot slot BEFORE the spawn so a second tick can't double-spawn the worker.
	_reconcileInFlight.store(true, std::memory_order_release);
	if (xTaskCreate(&ThreeCxAnchorClient::reconcileTaskTrampoline, "3cx_reconcile", 6144, this, 5, nullptr) != pdPASS)
	{
		// Rare error path (not the hot path): release the slot, else the watchdog wedges forever.
		ESP_LOGE(TAG, "tick: failed to spawn reconcile worker");
		_reconcileInFlight.store(false, std::memory_order_release);
	}
}

// One-shot worker: if _outboundActive has been stuck past the grace window, ask 3CX what is
// actually on the DN and clear the flag ONLY on definitive evidence the DN is empty. Off-SIP so
// the blocking GET is safe here.
void ThreeCxAnchorClient::reconcileTaskTrampoline(void* arg)
{
	auto* self = static_cast<ThreeCxAnchorClient*>(arg);

	std::string url;
	{
		std::lock_guard<std::mutex> lock(self->_mutex);
		url = self->_baseUrl + "/callcontrol/" + self->_sourceDn + "/participants";
	}

	ESP_LOGI(TAG, "reconcile watchdog: checking live participants on DN %s", self->_sourceDn.c_str());

	std::string body;
	int status = 0;
	const bool ok = self->httpGetBody(url, body, &status);

	// Only DEFINITIVE evidence — a clean 200 whose participant array is empty — clears the flag.
	// A failed/timed-out/non-200 GET is "no evidence" and must NOT clear it: a transient network
	// failure must never become a false all-clear that silently re-wedges inbound ringing.
	if (ok && status == 200)
	{
		bool emptyDn = false;
		cJSON* root = cJSON_Parse(body.c_str());
		if (root)
		{
			CJsonDeleter deleter{root};
			emptyDn = cJSON_IsArray(root) && (cJSON_GetArraySize(root) == 0);
		}
		if (emptyDn)
		{
			std::lock_guard<std::mutex> lock(self->_mutex);
			self->_outboundActive.store(false, std::memory_order_release);
			self->_inboundSignaledPartId.clear();
			ESP_LOGI(TAG, "reconcile watchdog: DN empty — cleared wedged _outboundActive");
		}
	}
	else
	{
		ESP_LOGW(TAG, "reconcile watchdog: GET inconclusive (ok=%d status=%d) — flag unchanged",
		         ok ? 1 : 0, status);
	}

	// Single exit: clear the one-shot slot so tick() can re-arm, THEN self-delete. vTaskDelete()
	// does not unwind the C++ stack, so this clear must be explicit here (not an RAII guard) — and
	// the only cJSON RAII above is confined to an inner scope that has already run.
	self->_reconcileInFlight.store(false, std::memory_order_release);
	vTaskDelete(nullptr);
}

// Issue #65 (L-1): one-shot worker that reclaims leaked GET sockets by cycling the
// whole anchor. stop() tears down the WS + control + media clients and frees every
// socket it still OWNS; the leaked _getClient handles cannot be closed (their mutex is
// poisoned), but a full stop()/start() drops the anchor's live socket footprint to
// zero and rebuilds it clean, so the pool recovers headroom. Runs off the SIP task
// because stop()/start() block on TLS teardown/handshake.
void ThreeCxAnchorClient::restartTaskTrampoline(void* arg)
{
	auto* self = static_cast<ThreeCxAnchorClient*>(arg);

	ESP_LOGW(TAG, "anchor restart: reclaiming socket pool after %d leaked GET handle(s)",
	         self->_leakedGetClients.load(std::memory_order_relaxed));

	// Clear the request BEFORE the cycle so a leak that happens during/after the
	// restart re-arms the watchdog instead of being swallowed.
	self->_restartRequested.store(false, std::memory_order_release);

	// stop() nulls _eventCb/_audioCb (they are installed once at boot by the engine
	// wiring and never re-installed by start()). Snapshot them under _mutex and
	// re-register after stop() so the reconnected anchor still delivers Incoming events
	// and inbound audio — otherwise the restart would leave the anchor deaf.
	EventCallback   savedEventCb;
	AudioRxCallback savedAudioCb;
	{
		std::lock_guard<std::mutex> lock(self->_mutex);
		savedEventCb = self->_eventCb;
		savedAudioCb = self->_audioCb;
	}

	self->stop();
	// Reset the leak tally: stop() has released everything the anchor owned, so the
	// previously-leaked handles are the OS's problem now and the fresh session starts
	// from a clean count. (A still-poisoned mutex would re-leak and re-trip the gate.)
	self->_leakedGetClients.store(0, std::memory_order_release);

	// Re-install the callbacks before start() so the new WS session is wired up.
	self->setEventCallback(savedEventCb);
	self->registerAudioRxCallback(savedAudioCb);

	if (!self->start())
	{
		ESP_LOGE(TAG, "anchor restart: start() failed — will retry on the next reconnect cycle");
	}
	else
	{
		ESP_LOGI(TAG, "anchor restart: complete, socket pool reclaimed");
	}

	// Clear the in-flight gate LAST so tick() can spawn a future restart if needed.
	self->_restartInFlight.store(false, std::memory_order_release);
	vTaskDelete(nullptr);
}

esp_http_client_handle_t ThreeCxAnchorClient::makeAuthedClient(const std::string& url, esp_http_client_method_t method, int txBufSize, const std::string& token)
{
	esp_http_client_config_t config = {};
	config.url = url.c_str();
	config.method = method;
	config.crt_bundle_attach = esp_crt_bundle_attach;
	config.buffer_size = 4096;
	config.buffer_size_tx = txBufSize;
	config.timeout_ms = 2000; // 2 seconds timeout to bound blocking calls

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

bool ThreeCxAnchorClient::performCtrl(const std::string& url, const char* contentType, const std::string& body, int* statusCodeOut)
{
	std::string token;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		token = _accessToken;
	}

	std::lock_guard<std::mutex> ctrlLock(_ctrlMutex);

	// Two attempts: the first may ride a connection the server idled out, in
	// which case perform() fails; the second goes out on a fresh handle, which
	// is exactly what every request paid before this connection was persistent.
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		if (!_ctrlClient)
		{
			_ctrlClient = makeAuthedClient(url, HTTP_METHOD_POST, 1024, token);
			if (!_ctrlClient)
			{
				return false;
			}
		}
		else
		{
			esp_http_client_set_url(_ctrlClient, url.c_str());
			esp_http_client_set_method(_ctrlClient, HTTP_METHOD_POST);
			// Token may have rotated since the handle was created.
			std::string authHeader = "Bearer " + token;
			esp_http_client_set_header(_ctrlClient, "Authorization", authHeader.c_str());
		}

		if (contentType)
		{
			esp_http_client_set_header(_ctrlClient, "Content-Type", contentType);
		}
		esp_http_client_set_post_field(_ctrlClient, body.c_str(), body.length());

		esp_err_t err = esp_http_client_perform(_ctrlClient);
		if (err == ESP_OK)
		{
			int status = esp_http_client_get_status_code(_ctrlClient);
			if (statusCodeOut)
			{
				*statusCodeOut = status;
			}
			return (status >= 200 && status < 300);
		}

		ESP_LOGW(TAG, "Control request failed (%s)%s", esp_err_to_name(err),
		         attempt == 0 ? " — reconnecting" : "");
		esp_http_client_cleanup(_ctrlClient);
		_ctrlClient = nullptr;
	}

	if (statusCodeOut)
	{
		*statusCodeOut = -1;
	}
	return false;
}

void ThreeCxAnchorClient::warmCtrlConnection()
{
	// Establish the control-plane TLS session ahead of the first command so a
	// makecall right after dialing doesn't pay the handshake. A POST to the DN
	// root is a no-op command-wise (3CX answers 4xx) but completes the TLS+TCP
	// setup that perform() will then reuse.
	std::string url;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		url = _baseUrl + "/callcontrol/" + _sourceDn;
	}
	int status = 0;
	performCtrl(url, nullptr, "", &status);
	ESP_LOGI(TAG, "Control connection pre-warmed (HTTP %d)", status);
}

void ThreeCxAnchorClient::closeCtrlClient()
{
	std::lock_guard<std::mutex> ctrlLock(_ctrlMutex);
	if (_ctrlClient)
	{
		esp_http_client_cleanup(_ctrlClient);
		_ctrlClient = nullptr;
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
			{
				// A device registration flap can invalidate the cached device_id — drop it so the
				// next makeCall re-resolves rather than POSTing to a stale /devices/{id}/makecall.
				std::lock_guard<std::mutex> lock(_mutex);
				_deviceId.clear();
			}
			stopMediaStreams();
			break;

		case WEBSOCKET_EVENT_DATA:
			// Teardown gate: once stop()/disable has cleared _running, ignore further frames so a
			// late upsert cannot prime a new rx task (startRxIfNeeded) against a tearing-down client.
			if (!_running.load(std::memory_order_acquire)) break;
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
				CJsonDeleter deleter{root};

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
									// 3CX repeats Upset for a participant every ~750ms. Skip the
									// work only once the call is FULLY up for this participant
									// (POST stream open => Answered/200 OK already fired). The Rx
									// task alone no longer counts as busy — it is primed during
									// ringing below, and keying the throttle on it would swallow
									// the Connected Upset that actually answers the call.
									// The leg WE control: outbound -> the makecall-selected own leg
									// (_activeParticipantId, set from result.id); inbound -> this partId.
									// 3CX may surface the FAR leg here, on which a specific-id GET/drop
									// 403s (issue #40), so for outbound we act on our own leg, not partId.
									bool outbound = _outboundActive.load(std::memory_order_acquire);
									std::string controlLeg;
									{
										std::lock_guard<std::mutex> lock(_mutex);
										controlLeg = _activeParticipantId;
									}
									if (outbound)
									{
										if (controlLeg.empty())
										{
											// makeCall hasn't resolved our own leg yet — ignore this
											// (likely far-leg) upset; a repeat upset drives us once set.
											break;
										}
									}
									else
									{
										controlLeg = partId;   // inbound: the surfaced leg is ours
									}

									bool sameLeg = false;
									{
										std::lock_guard<std::mutex> lock(_mutex);
										sameLeg = (controlLeg == _activeParticipantId);
									}
									if (sameLeg && isStreaming())
									{
										break;   // call already fully up
									}

									// Authoritative status from the LIST (GET /participants -> find leg),
									// never getParticipantStatus(id) which 403s for a non-controlled leg.
									std::string statusStr = getLegStatus(controlLeg);
									ESP_LOGI(TAG, "Upset %s -> control leg %s status '%s'", partId.c_str(), controlLeg.c_str(), statusStr.c_str());

									if (statusStr == "Connected")
									{
										// Control + media key off OUR leg (controlLeg), not the
										// surfaced partId — the latter can be the far leg (403).
										if (startMediaStreams(controlLeg))
										{
											if (evCb)
											{
												CallEvent ev{CallEvent::Answered, controlLeg, "", ""};
												evCb(ev);
											}
										}
										else
										{
											ESP_LOGE(TAG, "Failed to start media streams for participant %s", controlLeg.c_str());
											stopMediaStreams();
										}
									}
									else
									{
										// A not-yet-connected participant on our DN is either our own
										// outbound leg ringing, OR the upstream delivering a PSTN call
										// to the monitored DN. Disambiguate on _outboundActive.
										bool isInbound = false;
										{
											std::lock_guard<std::mutex> lock(_mutex);
											isInbound = !_outboundActive.load(std::memory_order_acquire) &&
											            _inboundSignaledPartId != partId;
											if (isInbound) _inboundSignaledPartId = partId;
										}

										if (isInbound)
										{
											// INBOUND: announce it once so the engine rings the local
											// extension. It calls answerCall(partId) on local answer,
											// which flips the leg to Connected (handled above). Pull a
											// best-effort caller id from attached_data for the From line.
											std::string callerId;
											cJSON* attached = cJSON_GetObjectItem(eventObj, "attached_data");
											if (attached)
											{
												for (const char* k : { "caller_id", "party_caller_id", "callerid", "caller_number" })
												{
													cJSON* c = cJSON_GetObjectItem(attached, k);
													if (c && c->valuestring && c->valuestring[0])
													{
														callerId = c->valuestring;
														break;
													}
												}
											}
											ESP_LOGI(TAG, "Inbound call on DN %s: participant %s caller '%s'",
											         _sourceDn.c_str(), partId.c_str(), callerId.c_str());
											if (evCb)
											{
												CallEvent ev{CallEvent::Incoming, partId, "", callerId};
												evCb(ev);
											}
										}

										// RING-TIME PRIME on the leg we control (own leg for outbound,
										// partId for inbound) so the GET stream is polling before connect.
										startRxIfNeeded(controlLeg);
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
										CallEvent ev{CallEvent::Dropped, partId, "", ""};
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
											CallEvent ev{CallEvent::Dtmf, partId, dtmfInput->valuestring, ""};
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

bool ThreeCxAnchorClient::startRxIfNeeded(const std::string& participantId)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (_rxTaskHandle != nullptr)
	{
		// Already polling (ring-time prime or a duplicate call) — nothing to do.
		return true;
	}
	_activeParticipantId = participantId;
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
		return false;
	}
	ESP_LOGI(TAG, "Rx stream task started for participant %s", participantId.c_str());
	return true;
}

bool ThreeCxAnchorClient::startMediaStreams(const std::string& participantId)
{
	ESP_LOGI(TAG, "Starting media streams for participant %s", participantId.c_str());

	// Single-call guard: a live POST stream means a call is already fully up.
	// (The Rx task alone is NOT a busy signal anymore — it gets primed during
	// ringing by startRxIfNeeded.)
	{
		std::lock_guard<std::mutex> postLock(_postMutex);
		if (_postClient != nullptr)
		{
			ESP_LOGW(TAG, "startMediaStreams: Media streams already active");
			return false;
		}
	}

	std::string baseUrl, sourceDn, token;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_rxTaskHandle != nullptr && _activeParticipantId != participantId)
		{
			ESP_LOGW(TAG, "startMediaStreams: Rx busy with participant %s", _activeParticipantId.c_str());
			return false;
		}
		_activeParticipantId = participantId;
		baseUrl = _baseUrl;
		sourceDn = _sourceDn;
		token = _accessToken;
	}

	// 1. Make sure the Rx task is up (no-op when it was primed at ringing) so
	// its GET-stream handshake/polling overlaps the POST open below.
	if (!startRxIfNeeded(participantId))
	{
		return false;
	}

	// 2. Initialize HTTP POST client for outbound audio. On failure the caller
	// invokes stopMediaStreams(), which shuts down and joins the Rx task.
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

	return true;
}

void ThreeCxAnchorClient::stopMediaStreams()
{
	// Single-entry gate: a concurrent caller (WS-disconnect vs stop()) loses the CAS and returns
	// at once, so only the winner reaches the _getClient free below. (Invariant: the rx task reads
	// _getClient only while _running is true and not tearing down, so the winner is its sole owner
	// here.) RAII clears the gate on every return path — safe because this is a normal function,
	// not a self-deleting task.
	bool expected = false;
	if (!_tearingDown.compare_exchange_strong(expected, true))
	{
		return;
	}
	struct ClearOnExit {
		std::atomic<bool>& f;
		~ClearOnExit() { f.store(false, std::memory_order_release); }
	} clearOnExit{_tearingDown};

	TaskHandle_t taskToKill = nullptr;
	SemaphoreHandle_t doneSem = nullptr;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_activeParticipantId.clear();
		// Call is over: clear the inbound/outbound disambiguation so the NEXT participant
		// upsert is classified fresh (a stale _outboundActive would mask a real inbound
		// call; a stale _inboundSignaledPartId would swallow its Incoming event).
		_outboundActive.store(false, std::memory_order_release);
		_inboundSignaledPartId.clear();
		taskToKill = _rxTaskHandle;
		// Capture the semaphore under the SAME lock: startRxIfNeeded deletes+recreates _rxDoneSem,
		// so re-reading the member after releasing the lock could race a fresh handle (UAF). Use
		// the local 'doneSem' for the whole teardown below, never _rxDoneSem.
		doneSem = _rxDoneSem;
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

		if (doneSem)
		{
			if (xSemaphoreTake(doneSem, pdMS_TO_TICKS(2000)) != pdTRUE)
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
					// Issue #65 (L-1): _getClient (and its LWIP socket) is now unrecoverable
					// in place. Count it; once we have leaked too many, request a full anchor
					// restart so the next stop()/start() reclaims the whole socket pool. The
					// restart is performed off the SIP task by tick() (it only spawns a worker).
					const int leaked = _leakedGetClients.fetch_add(1, std::memory_order_relaxed) + 1;
					ESP_LOGE(TAG, "_getMutex poisoned by killed task — leaking _getClient to avoid deadlock (%d leaked)", leaked);
					if (leaked >= kLeakRestartThreshold)
					{
						_restartRequested.store(true, std::memory_order_release);
						ESP_LOGW(TAG, "Leaked GET sockets reached %d — requesting anchor restart to reclaim the pool", leaked);
					}
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
	//
	// LATENCY: the client handle is created ONCE and re-opened across retries so
	// the TLS session persists — the 3CX reference examples do the same with
	// keep-alive agents. The old loop tore the client down per attempt, paying a
	// full mbedTLS handshake (~0.5-1s on the S3) per 404, which serialized into
	// multi-second answer-to-audio delays. Now only the first attempt (or a
	// server-closed connection) pays a handshake; a retry is ~one RTT.
	// 240 attempts ≈ 2 min at the 500ms backoff cap: the task is now started at
	// RINGING, so the loop must comfortably outlast a long unanswered ring (the
	// old 40-attempt/~20s ceiling would expire mid-ring and the call would
	// connect with no inbound audio). Teardown still exits it immediately via
	// _rxTaskHandle/socket shutdown.
	constexpr int        kMaxAttempts = 240;
	TickType_t           delay        = pdMS_TO_TICKS(50);   // Start fast at 50ms
	bool opened = false;

	{
		std::lock_guard<std::mutex> lock(_getMutex);
		if (_rxTaskHandle != nullptr)
		{
			_getClient = makeAuthedClient(getUrl, HTTP_METHOD_GET, 1024, token);
		}
	}

	if (_getClient)
	{
		for (int attempt = 0; attempt < kMaxAttempts && _rxTaskHandle != nullptr; ++attempt)
		{
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
				// Drain the error body completely so the persistent connection can
				// carry the next attempt (an unread body poisons handle reuse).
				char drainBuf[256];
				while (esp_http_client_read(_getClient, drainBuf, sizeof(drainBuf)) > 0) {}
			}
			else
			{
				// Connection-level failure: esp_http_client re-establishes the
				// transport on the next open, so still no per-retry teardown needed.
				ESP_LOGW(TAG, "GET stream open failed (%s), attempt %d/%d", esp_err_to_name(err), attempt + 1, kMaxAttempts);
			}

			if (_rxTaskHandle != nullptr)
			{
				vTaskDelay(delay);
				delay = (delay * 2 > pdMS_TO_TICKS(500)) ? pdMS_TO_TICKS(500) : delay * 2;
			}
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
