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
#include "cJSON.h"
#include <sstream>
#include <cstring>
#include <vector>
#include <cmath>
#include "esp_crt_bundle.h"

static const char* TAG = "ThreeCxAnchor";

static void play_tone_task(void* arg);

ThreeCxAnchorClient::ThreeCxAnchorClient() = default;

ThreeCxAnchorClient::~ThreeCxAnchorClient()
{
	stop();
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
	std::lock_guard<std::mutex> lock(_mutex);
	if (_running)
	{
		return false;
	}

	_running = true;

	// 1. Fetch OAuth token
	if (!fetchToken())
	{
		ESP_LOGE(TAG, "Failed to retrieve OAuth token");
		_running = false;
		return false;
	}

	// 2. Connect to control WebSocket
	if (!connectWs())
	{
		ESP_LOGE(TAG, "Failed to connect control WebSocket");
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
	std::lock_guard<std::mutex> lock(_mutex);
	return _postClient != nullptr;
}

bool ThreeCxAnchorClient::makeCall(const std::string& destination)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_connected.load(std::memory_order_acquire))
	{
		ESP_LOGW(TAG, "Cannot make call: not connected to 3CX");
		return false;
	}

	// Trigger outbound call via HTTP POST /callcontrol/{sourceDn}/makecall
	std::string makeCallUrl = _baseUrl + "/callcontrol/" + _sourceDn + "/makecall";

	esp_http_client_config_t config = {};
	config.url = makeCallUrl.c_str();
	config.method = HTTP_METHOD_POST;
	config.crt_bundle_attach = esp_crt_bundle_attach;
	config.buffer_size = 4096;
	config.buffer_size_tx = 1024;

	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (!client)
	{
		ESP_LOGE(TAG, "Failed to init HTTP client for makeCall");
		return false;
	}

	std::string authHeader = "Bearer " + _accessToken;
	esp_http_client_set_header(client, "Authorization", authHeader.c_str());
	esp_http_client_set_header(client, "Content-Type", "application/json");

	std::string postData = "{\"destination\":\"" + destination + "\"}";
	esp_http_client_set_post_field(client, postData.c_str(), postData.length());

	esp_err_t err = esp_http_client_perform(client);
	bool success = false;
	if (err == ESP_OK)
	{
		int status = esp_http_client_get_status_code(client);
		if (status >= 200 && status < 300)
		{
			success = true;
			ESP_LOGI(TAG, "Successfully initiated call to %s", destination.c_str());
		}
		else
		{
			ESP_LOGE(TAG, "makeCall returned HTTP status %d", status);
		}
	}
	else
	{
		ESP_LOGE(TAG, "makeCall HTTP POST request failed: %s", esp_err_to_name(err));
	}

	esp_http_client_cleanup(client);
	return success;
}

bool ThreeCxAnchorClient::dropCall(const std::string& participantId)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_connected.load(std::memory_order_acquire))
	{
		return false;
	}

	std::string partId = participantId.empty() ? _activeParticipantId : participantId;
	if (partId.empty())
	{
		return false;
	}

	// Trigger drop via HTTP POST /callcontrol/{sourceDn}/participants/{participantId}/drop
	std::string dropUrl = _baseUrl + "/callcontrol/" + _sourceDn + "/participants/" + partId + "/drop";

	esp_http_client_config_t config = {};
	config.url = dropUrl.c_str();
	config.method = HTTP_METHOD_POST;
	config.crt_bundle_attach = esp_crt_bundle_attach;
	config.buffer_size = 4096;
	config.buffer_size_tx = 1024;

	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (!client)
	{
		return false;
	}

	std::string authHeader = "Bearer " + _accessToken;
	esp_http_client_set_header(client, "Authorization", authHeader.c_str());

	esp_err_t err = esp_http_client_perform(client);
	bool success = false;
	if (err == ESP_OK)
	{
		int status = esp_http_client_get_status_code(client);
		if (status >= 200 && status < 300)
		{
			success = true;
			ESP_LOGI(TAG, "Successfully dropped participant %s", participantId.c_str());
		}
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

	const char* rawBytes = reinterpret_cast<const char*>(pcmSamples);
	size_t rawLen = count * sizeof(int16_t);

	int written = esp_http_client_write(_postClient, rawBytes, rawLen);
	return (written >= 0);
}

void ThreeCxAnchorClient::registerAudioRxCallback(AudioRxCallback cb)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_audioCb = cb;
}

// ── Private Helper Functions ───────────────────────────────────────────────

bool ThreeCxAnchorClient::fetchToken()
{
	std::string tokenUrl = _baseUrl + "/connect/token";

	esp_http_client_config_t config = {};
	config.url = tokenUrl.c_str();
	config.method = HTTP_METHOD_POST;
	config.crt_bundle_attach = esp_crt_bundle_attach;
	config.buffer_size = 4096;
	config.buffer_size_tx = 1024;

	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (!client)
	{
		ESP_LOGE(TAG, "Failed to init HTTP client for fetchToken");
		return false;
	}

	esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");

	std::string body = "grant_type=client_credentials&client_id=" + _clientId +
	                   "&client_secret=" + _clientSecret;

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
				std::vector<char> buffer;
				char tempBuf[512];
				int readBytes = 0;
				// Loop reads into an accumulator until EOF (0) or error (<0)
				while ((readBytes = esp_http_client_read(client, tempBuf, sizeof(tempBuf))) > 0)
				{
					buffer.insert(buffer.end(), tempBuf, tempBuf + readBytes);
				}
				if (readBytes < 0)
				{
					ESP_LOGE(TAG, "Token read error: %d", readBytes);
				}
				else if (!buffer.empty())
				{
					buffer.push_back('\0');
					cJSON* root = cJSON_Parse(buffer.data());
					if (root)
					{
						cJSON* tokenItem = cJSON_GetObjectItem(root, "access_token");
						if (tokenItem && tokenItem->valuestring)
						{
							_accessToken = tokenItem->valuestring;
							ESP_LOGI(TAG, "Retrieved access token (len=%d)", (int)_accessToken.length());
							success = true;
						}
						cJSON_Delete(root);
					}
					else
					{
						ESP_LOGE(TAG, "Failed to parse JSON response: %s", buffer.data());
					}
				}
				else
				{
					ESP_LOGE(TAG, "Empty token response body");
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

bool ThreeCxAnchorClient::connectWs()
{
	// Convert base https:// URL to wss:// for call control websocket
	std::string wsUrl = _baseUrl;
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

	esp_websocket_client_config_t wsCfg = {};
	wsCfg.uri = wsUrl.c_str();
	wsCfg.crt_bundle_attach = esp_crt_bundle_attach;
	wsCfg.task_stack = 16384; // needs headroom for two blocking HTTP sessions fired from event handler
	wsCfg.buffer_size = 4096;

	_wsClient = esp_websocket_client_init(&wsCfg);
	if (!_wsClient)
	{
		return false;
	}

	// Pass Authorization header using Bearer token
	std::string authHeader = "Authorization: Bearer " + _accessToken + "\r\n";
	esp_websocket_client_set_headers(_wsClient, authHeader.c_str());

	esp_err_t err = esp_websocket_register_events(_wsClient, WEBSOCKET_EVENT_ANY,
	                                               &ThreeCxAnchorClient::wsEventTrampoline, this);
	if (err != ESP_OK)
	{
		esp_websocket_client_destroy(_wsClient);
		_wsClient = nullptr;
		return false;
	}

	err = esp_websocket_client_start(_wsClient);
	if (err != ESP_OK)
	{
		esp_websocket_client_destroy(_wsClient);
		_wsClient = nullptr;
		return false;
	}

	return true;
}

std::string ThreeCxAnchorClient::getParticipantStatus(const std::string& participantId)
{
	std::string getUrl = _baseUrl + "/callcontrol/" + _sourceDn + "/participants/" + participantId;

	esp_http_client_config_t config = {};
	config.url = getUrl.c_str();
	config.method = HTTP_METHOD_GET;
	config.crt_bundle_attach = esp_crt_bundle_attach;
	config.buffer_size = 4096;
	config.buffer_size_tx = 1024;

	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (!client)
	{
		ESP_LOGE(TAG, "Failed to init HTTP client for getParticipantStatus");
		return "";
	}

	std::string authHeader = "Bearer " + _accessToken;
	esp_http_client_set_header(client, "Authorization", authHeader.c_str());

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
			std::vector<char> buffer;
			char tempBuf[256];
			int readBytes = 0;
			// Loop reads into an accumulator until EOF (0) or error (<0)
			while ((readBytes = esp_http_client_read(client, tempBuf, sizeof(tempBuf))) > 0)
			{
				buffer.insert(buffer.end(), tempBuf, tempBuf + readBytes);
			}
			if (readBytes < 0)
			{
				ESP_LOGE(TAG, "getParticipantStatus read error: %d", readBytes);
			}
			else if (!buffer.empty())
			{
				buffer.push_back('\0');
				cJSON* root = cJSON_Parse(buffer.data());
				if (root)
				{
					cJSON* statusItem = cJSON_GetObjectItem(root, "status");
					if (statusItem && statusItem->valuestring)
					{
						statusStr = statusItem->valuestring;
					}
					cJSON_Delete(root);
				}
			}
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
			if (data->op_code == 0x01 && data->data_ptr != nullptr && data->data_len > 0)
			{
				// Received text data from WSS
				std::string payload(data->data_ptr, data->data_len);
				cJSON* root = cJSON_Parse(payload.c_str());
				if (!root) break;

				cJSON* eventObj = cJSON_GetObjectItem(root, "event");
				if (eventObj)
				{
					cJSON* evType = cJSON_GetObjectItem(eventObj, "event_type");
					cJSON* entity = cJSON_GetObjectItem(eventObj, "entity");

					if (evType && evType->valuestring && entity && entity->valuestring)
					{
						std::string typeStr = evType->valuestring;
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

								if (typeStr == "Upset")
								{
									ESP_LOGI(TAG, "Call control event: Participant Upset %s", partId.c_str());
									std::string statusStr = getParticipantStatus(partId);
									ESP_LOGI(TAG, "Participant %s status: %s", partId.c_str(), statusStr.c_str());

									if (statusStr == "Connected")
									{
										if (_activeParticipantId.empty())
										{
											startMediaStreams(partId);
										}

										if (evCb)
										{
											CallEvent ev{CallEvent::Answered, partId, ""};
											evCb(ev);
										}
									}
								}
								else if (typeStr == "Remove")
								{
									ESP_LOGI(TAG, "Call control event: Participant Remove %s", partId.c_str());
									if (_activeParticipantId == partId)
									{
										stopMediaStreams();
									}

									if (evCb)
									{
										CallEvent ev{CallEvent::Dropped, partId, ""};
										evCb(ev);
									}
								}
								else if (typeStr == "DTMFstring")
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
				cJSON_Delete(root);
			}
			break;

		default:
			break;
	}
}

bool ThreeCxAnchorClient::startMediaStreams(const std::string& participantId)
{
	ESP_LOGI(TAG, "Starting media streams for participant %s", participantId.c_str());
	_activeParticipantId = participantId;

	// 1. Initialize HTTP POST client for outbound audio
	std::string postUrl = _baseUrl + "/callcontrol/" + _sourceDn + "/participants/" + participantId + "/stream";
	esp_http_client_config_t postCfg = {};
	postCfg.url = postUrl.c_str();
	postCfg.method = HTTP_METHOD_POST;
	postCfg.crt_bundle_attach = esp_crt_bundle_attach;
	postCfg.buffer_size = 4096;
	postCfg.buffer_size_tx = 4096; // streaming POST chunked needs large tx buffer

	_postClient = esp_http_client_init(&postCfg);
	if (!_postClient)
	{
		return false;
	}

	std::string authHeader = "Bearer " + _accessToken;
	esp_http_client_set_header(_postClient, "Authorization", authHeader.c_str());
	esp_http_client_set_header(_postClient, "Content-Type", "application/octet-stream");
	esp_http_client_set_header(_postClient, "Transfer-Encoding", "chunked");

	esp_err_t err = esp_http_client_open(_postClient, 0); // open chunked request
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed to open chunked HTTP POST stream: %s", esp_err_to_name(err));
		esp_http_client_cleanup(_postClient);
		_postClient = nullptr;
		return false;
	}

	// 2. Initialize HTTP GET client and launch Rx task for inbound audio
	if (_rxDoneSem)
	{
		vSemaphoreDelete(_rxDoneSem);
	}
	_rxDoneSem = xSemaphoreCreateBinary();
	xTaskCreatePinnedToCore(&ThreeCxAnchorClient::rxTaskTrampoline, "3cx_media_rx", 6144, this, 6, &_rxTaskHandle, 1);

	// 3. Launch Tone generation task to play a tone on answer
	xTaskCreatePinnedToCore(&play_tone_task, "3cx_media_tx_tone", 3072, this, 5, nullptr, 1);

	return true;
}

void ThreeCxAnchorClient::stopMediaStreams()
{
	_activeParticipantId.clear();

	if (_rxTaskHandle)
	{
		// Signal the rx loop to exit, then close the connection so the blocking
		// read() call returns immediately. Wait for the task to finish cleaning
		// up _getClient before proceeding — without this barrier the task and
		// this thread race on the handle (double-free / use-after-free).
		_rxTaskHandle = nullptr;
		if (_getClient)
		{
			esp_http_client_close(_getClient); // unblocks runRxLoop's read()
		}
		if (_rxDoneSem)
		{
			xSemaphoreTake(_rxDoneSem, pdMS_TO_TICKS(2000));
		}
		// runRxLoop() called cleanup and nulled _getClient on its exit path.
	}
	else if (_getClient)
	{
		// No rx task running — safe to clean up directly.
		esp_http_client_close(_getClient);
		esp_http_client_cleanup(_getClient);
		_getClient = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(_postMutex);
		if (_postClient)
		{
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

static void play_tone_task(void* arg)
{
	auto* self = static_cast<ThreeCxAnchorClient*>(arg);
	float phase = 0;
	float phaseStep = 2.0f * 3.14159265f * 440.0f / 8000.0f;
	TickType_t lastWakeTime = xTaskGetTickCount();

	ESP_LOGI("ToneTask", "Starting tone generation task");
	while (self->isConnected() && self->isStreaming())
	{
		int16_t toneSamples[160];
		for (int i = 0; i < 160; ++i)
		{
			toneSamples[i] = (int16_t)(10000.0f * std::sin(phase));
			phase += phaseStep;
			if (phase >= 2.0f * 3.14159265f)
			{
				phase -= 2.0f * 3.14159265f;
			}
		}

		self->writeAudio(toneSamples, 160);
		vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(20));
	}
	ESP_LOGI("ToneTask", "Tone generation task stopped");
	vTaskDelete(nullptr);
}

void ThreeCxAnchorClient::runRxLoop()
{
	std::string getUrl = _baseUrl + "/callcontrol/" + _sourceDn + "/participants/" + _activeParticipantId + "/stream";
	esp_http_client_config_t getCfg = {};
	getCfg.url = getUrl.c_str();
	getCfg.method = HTTP_METHOD_GET;
	getCfg.crt_bundle_attach = esp_crt_bundle_attach;
	getCfg.buffer_size = 4096;
	getCfg.buffer_size_tx = 1024;

	_getClient = esp_http_client_init(&getCfg);
	if (!_getClient)
	{
		return;
	}

	std::string authHeader = "Bearer " + _accessToken;
	esp_http_client_set_header(_getClient, "Authorization", authHeader.c_str());

	esp_err_t err = esp_http_client_open(_getClient, 0);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed to open HTTP GET stream: %s", esp_err_to_name(err));
		esp_http_client_cleanup(_getClient);
		_getClient = nullptr;
		return;
	}

	esp_http_client_fetch_headers(_getClient);
	int status = esp_http_client_get_status_code(_getClient);
	if (status != 200)
	{
		ESP_LOGE(TAG, "HTTP GET stream returned status %d", status);
		esp_http_client_cleanup(_getClient);
		_getClient = nullptr;
		return;
	}

	char readBuf[512];
	while (_rxTaskHandle != nullptr)
	{
		int bytesRead = esp_http_client_read(_getClient, readBuf, sizeof(readBuf));
		if (bytesRead <= 0)
		{
			// End of stream or connection closed
			break;
		}

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

	esp_http_client_close(_getClient);
	esp_http_client_cleanup(_getClient);
	_getClient = nullptr;
}

#endif
