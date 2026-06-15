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
#include "freertos/queue.h"   // #43: WS event work queue
#include "freertos/semphr.h"  // #43: worker-pool done semaphore (+ existing _rxDoneSem)
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
	bool answerCall(const std::string& participantId) override;
	bool dropCall(const std::string& participantId) override;
	void setEventCallback(EventCallback cb) override;
	bool writeAudio(const int16_t* pcmSamples, size_t count) override;
	void registerAudioRxCallback(AudioRxCallback cb) override;
	void tick() override;   // non-blocking; runs the _outboundActive reconcile watchdog (ESP only)

private:
	std::string _baseUrl;
	std::string _clientId;
	std::string _clientSecret;
	std::string _sourceDn;

	std::atomic<bool> _running{false};
	std::atomic<bool> _connected{false};
	std::string       _accessToken;
	std::string       _activeParticipantId;

	// Inbound disambiguation. A participant upsert on _sourceDn is OURS (outbound) when
	// _outboundActive is set by makeCall; otherwise it is the upstream delivering a PSTN
	// call to the monitored DN. _inboundSignaledPartId records the participant we have
	// already announced via CallEvent::Incoming so the ~750 ms upsert repeats fire it
	// only once. Both guarded by _mutex.
	std::atomic<bool> _outboundActive{false};
	// One-shot: has CallEvent::Answered already fired for the current outbound leg? Media is
	// now pre-warmed during dialing (so audio cuts through at connect), which makes the leg
	// "streaming" BEFORE it is Connected — so isStreaming() can no longer stand in for
	// "answered." This flag fires Answered exactly once when the leg first reaches Connected.
	// Reset by makeCall (new call) and stopMediaStreams (teardown); unused on the inbound path.
	std::atomic<bool> _outboundAnswered{false};
	std::string       _inboundSignaledPartId;
	// Monotonic (esp_timer) timestamp of the last _outboundActive=true, guarded by _mutex.
	// The tick() watchdog uses it to detect an outbound makecall that 3CX accepted but that
	// never produced a participant — the flag would otherwise wedge inbound ringing forever.
	int64_t           _outboundActiveSetUs = 0;
	// One-shot guard: the watchdog spawns at most one reconcile worker at a time.
	std::atomic<bool> _reconcileInFlight{false};
	// Monotonic (esp_timer) timestamp of the last reconcile spawn. #94: the reconcile GET
	// fails with sock<0 while a call holds the TLS sockets, so without a floor the watchdog
	// re-spawned every 1 Hz tick — busy-looping failed connects and ADDING socket pressure.
	// tick() enforces a minimum interval between spawns. Atomic: read on the SIP task,
	// written by the reconcile worker.
	std::atomic<int64_t> _lastReconcileUs{0};
	// Resolved device_id for the device-specific makecall transport, guarded by _mutex. Empty
	// until lazily resolved; cleared on WS disconnect so a device registration flap re-resolves.
	std::string       _deviceId;

	EventCallback   _eventCb;
	AudioRxCallback _audioCb;

	mutable std::mutex _mutex;
	mutable std::mutex _postMutex; // guards _postClient across writeAudio / stopMediaStreams; mutable for isStreaming() const
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

	// Persistent control-plane HTTPS connection (makecall / participant drop).
	// Kept open across requests so each command is one RTT instead of a fresh
	// mbedTLS handshake — mirrors the keep-alive agents in the 3CX reference
	// examples. Guarded by _ctrlMutex; never touched under _mutex.
	esp_http_client_handle_t      _ctrlClient = nullptr;
	std::mutex                    _ctrlMutex;

	TaskHandle_t      _rxTaskHandle = nullptr;
	SemaphoreHandle_t _rxDoneSem    = nullptr; // signalled by rx task before it deletes itself
	// Single-entry gate for stopMediaStreams(): a concurrent caller (WS-disconnect vs stop())
	// loses the CAS and returns immediately, so only the winner frees _getClient.
	std::atomic<bool> _tearingDown{false};

	// Issue #65 (L-1): on the rare 2 s join-timeout path stopMediaStreams() force-kills
	// the rx task with vTaskDelete; if that task was holding _getMutex it is permanently
	// poisoned and we deliberately LEAK _getClient (closing it under a poisoned mutex
	// could deadlock/double-free). Each leak burns one of the 16 LWIP sockets, so we
	// count them and, once kLeakRestartThreshold accumulate, request a full anchor
	// stop()/start() cycle to reclaim the socket pool. The restart runs OFF the SIP
	// task (tick() only spawns the worker — it never blocks).
	std::atomic<int>  _leakedGetClients{0};
	std::atomic<bool> _restartRequested{false};
	std::atomic<bool> _restartInFlight{false};
	// How many leaked GET sockets we tolerate before forcing a reclaiming restart. The
	// pool is 16 and the anchor itself needs 3 persistent sockets + SIP/dashboard/SSH,
	// so a handful of leaks is the safe ceiling before sockets start starving.
	static constexpr int kLeakRestartThreshold = 3;
	// One-shot worker that performs the full stop()/start() reclaim cycle off-SIP.
	static void restartTaskTrampoline(void* arg);

	static void wsEventTrampoline(void* handlerArgs, esp_event_base_t base, int32_t eventId, void* eventData);
	void handleWsEvent(int32_t eventId, void* eventData);

	// ── #43: WS event queue + worker pool ────────────────────────────────────────
	// The esp_websocket_client event task MUST stay responsive to keep answering PINGs
	// (a frozen WS task → missed PONGs → server declares the link dead → full reconnect).
	// So handleWsEvent() does only fast JSON-parse + gating, heap-allocs a WsWorkItem and
	// queues a POINTER (a FreeRTOS queue memcpy's its items — a std::string by value would
	// corrupt the heap), then returns. A worker POOL drains the queue and runs the blocking
	// getLegStatus()/startMediaStreams()/stopMediaStreams() off the WS task. kWsWorkers=1
	// today; >1 parallelises concurrent call setups for the multi-call future (issue #100) —
	// pool-ready by construction.
	enum class WsWork : uint8_t { Upset, Remove };
	struct WsWorkItem
	{
		WsWork      kind = WsWork::Upset;
		std::string controlLeg;   // the leg WE control (own outbound leg, or inbound partId)
		std::string partId;       // the participant 3CX surfaced in the event entity path
		std::string callerId;     // inbound From display name (best-effort)
	};
	static constexpr int kWsWorkers    = 1;    // bump for multi-call setup concurrency (#100)
	static constexpr int kWsQueueDepth = 16;
	QueueHandle_t     _wsWorkQueue = nullptr;
	TaskHandle_t      _wsWorkerHandles[kWsWorkers] = {};
	std::atomic<bool> _wsWorkersRun{false};
	// Counting sem each worker gives right before it self-deletes, so stopWsWorkers() can
	// wait for ALL workers to have left xQueueReceive() before it vQueueDelete()s the queue
	// (deleting a queue with a task blocked on it is UB). Created once, reused across restart.
	SemaphoreHandle_t _wsWorkerDoneSem = nullptr;
	bool startWsWorkers();
	void stopWsWorkers();
	void enqueueWsWork(WsWorkItem* item);          // takes ownership; deletes on enqueue failure
	static void wsWorkerTrampoline(void* arg);
	void runWsWorker();
	void processWsWork(const WsWorkItem& w);       // the blocking body, off the WS task

	bool fetchToken();
	bool ensureToken();             // refresh iff expiring AND no media streams active
	bool tokenExpiringSoon() const; // true when within the refresh margin of JWT exp
	bool connectWs();
	// getParticipantStatus() removed (chore #75): the specific-id GET 403s for a
	// non-controlled leg (issue #40). Use getLegStatus() (list-based) instead.

	static void rxTaskTrampoline(void* arg);
	void runRxLoop();

	bool startMediaStreams(const std::string& participantId);
	void stopMediaStreams();
	// Non-virtual teardown shared by stop() and the destructor so ~ThreeCxAnchorClient()
	// never makes a virtual call (cppcheck virtualCallInConstructor; dynamic binding is
	// not used in a destructor anyway).
	void shutdownImpl();
	// Spawn the Rx task (GET-stream retry loop) if it isn't running yet. Called
	// at RINGING so the loop is already polling on a warm TLS connection when
	// the leg connects — inbound audio then opens ~one RTT after answer.
	// Returns true if the task is running (newly spawned or already up).
	bool startRxIfNeeded(const std::string& participantId);

	esp_http_client_handle_t makeAuthedClient(const std::string& url, esp_http_client_method_t method, int txBufSize, const std::string& token = "");
	bool performAuthedRequest(esp_http_client_handle_t client, int* statusCodeOut = nullptr);
	// One-shot POST on the persistent control connection (creates/repairs the
	// handle as needed; retries once on a stale connection). contentType may be
	// nullptr for an empty-body POST.
	bool performCtrl(const std::string& url, const char* contentType, const std::string& body, int* statusCodeOut = nullptr);
	void warmCtrlConnection();   // pre-establish the control TLS session (boot/reconnect)
	void closeCtrlClient();      // teardown under _ctrlMutex
	bool readJsonStringField(esp_http_client_handle_t client, const std::string& field, std::string& out);

	// Live-state GET helpers (reconcile watchdog + drop-fallback + device resolve). Snapshot
	// creds under _mutex then do blocking I/O lock-free.
	bool httpGetBody(const std::string& url, std::string& bodyOut, int* statusOut = nullptr);
	// First participant id currently on our DN (single-leg assumption), or "" if none / on error.
	std::string reconcileParticipantId();
	// POST + capture the response body (for makecall result.id). Fresh client (not the persistent
	// ctrl connection) so the body is readable; creds snapshot under _mutex, blocking I/O lock-free.
	bool httpPostBody(const std::string& url, const char* contentType, const std::string& body, std::string& respBody, int* statusOut = nullptr);
	// The leg WE control for an outbound call: makecall result.id, else a destination digit-suffix
	// match in the live participant list — the id 3CX authorizes us to stream/drop (issue #40).
	std::string resolveOutboundLeg(const std::string& makecallRespBody, const std::string& destination);
	// Status of a specific leg read from the LIST (GET /participants -> find id). Replaces
	// getParticipantStatus(id), which 403s for a leg this DN cannot directly control (issue #40).
	std::string getLegStatus(const std::string& legId);
	// Best-effort caller display (party_caller_id, else party_caller_name) for an inbound leg
	// from the live participant list — the WS event's attached_data is often null on a route
	// point. "" if unavailable. Blocking GET; call off the WS event task.
	std::string getParticipantCaller(const std::string& legId);
	// Device-specific makecall transport.
	std::string pickDeviceId(const std::string& body);   // parse a /devices array → a device_id
	bool resolveDevice();                                 // GET /devices → pickDeviceId → _deviceId
	// One-shot worker spawned by tick() to clear a wedged _outboundActive (see .cpp).
	static void reconcileTaskTrampoline(void* arg);
#endif
};

#endif // THREECX_ANCHOR_CLIENT_HPP
