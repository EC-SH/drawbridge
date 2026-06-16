#ifndef REQUESTS_HANDLER_HPP
#define REQUESTS_HANDLER_HPP

// Gated open mode (Issue #56). Undefine/disable to run in closed/restricted mode.
#define POCKETDIAL_OPEN_REGISTRAR

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#elif defined(__linux__)
#include <netinet/in.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#endif

#include <functional>
#include <unordered_map>
#include <string>
#include <mutex>
#include <optional>
#include <vector>
#include <tuple>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <array>
#include <thread>
#include "SipMessage.hpp"
#include "SipClient.hpp"
#include "Session.hpp"
#include "CallDetailRecord.hpp"
#include "PbxConfig.hpp"
#include "RtpSender.hpp"
#include "RtpReceiver.hpp"
#include "MediaBridge.hpp"
#include "ThreeCxAnchorClient.hpp"
#include "LoopbackAnchorClient.hpp"
#include "TelephonyProvider.hpp"
#include "TelephonyApiConfig.hpp"

class RequestsHandler
{
public:

	using OnHandledEvent = std::function<void(const sockaddr_in&, std::shared_ptr<SipMessage>)>;

	RequestsHandler(std::string serverIp, int serverPort,
		OnHandledEvent onHandledEvent);
	~RequestsHandler();

	static std::shared_ptr<SipMessage> getMessageFromPool(std::string message, sockaddr_in src);

	// ── Media beachhead static helpers (pure; host-unit-tested) ──────────────────
	// Build the server's own SDP body for the 440 answer (server media: PCMU on the
	// server's RTP port). Pure formatter — exposed so tests can assert its body and
	// the resulting Content-Length correctness (the 777-bug class).
	static std::string buildMediaSdp(const std::string& serverIp, int rtpPort, bool sendRecv = false);

	// Parse the caller's RTP destination from an INVITE: the SDP c= line IP (falling
	// back to the INVITE source IP) + the m=audio port via getRtpPort(). Returns false
	// if no usable port is found. Pure/host-testable.
	static bool parseCallerRtp(const std::shared_ptr<SipMessage>& invite,
		std::string& outIp, uint16_t& outPort);

	// ── BLF/presence pure static helpers (host-unit-tested) ──────────────────────
	// Build a minimal RFC 4235 dialog-info+xml document. Always state="full" (every
	// NOTIFY carries the complete dialog set for the entity — one dialog max here).
	// `state` empty means "no active dialog": the <dialog> element is omitted, which
	// Yealink/Grandstream BLF keys render as an idle (off) lamp. `entity` is the
	// full SIP URI (e.g. "sip:101@192.168.4.1"); `state` is one of the RFC 4235
	// dialog states ("trying"|"early"|"confirmed"|"terminated").
	static std::string buildDialogInfoXml(const std::string& entity, unsigned version,
		const std::string& dialogId, const std::string& state, const std::string& direction);

	// Extract the Event package token from a raw SIP message: the value of the
	// Event: (or compact "o:") header up to any ';' parameter, trimmed. Returns ""
	// when the header is absent. Pure/host-testable.
	static std::string parseEventPackage(const std::string& raw);

	void handle(std::shared_ptr<SipMessage> request);
	void tick();

	std::optional<std::shared_ptr<Session>> getSession(std::string_view callID);

	// ── Dashboard query API (thread-safe) ────────────────────────────
	std::vector<std::pair<std::string, std::string>> getActiveClients();
	std::vector<std::tuple<std::string, std::string, std::string, int>> getActiveSessions();
	void forceDisconnect(const std::string& extension);
	uint64_t getPacketsProcessed() const;
	uint64_t getPacketsDropped() const;   // Issue #38: rate-limited/blocked packets
	size_t getClientCount();
	size_t getSessionCount();

	// Soak telemetry (issues #81/#83): one thread-safe snapshot of anchor/media/pool health
	// for /api/status. Reads are lock-free — atomics (anchor/media/playout) plus the existing
	// _snapshotMutex getters — so it never contends with the SIP thread under the registrar
	// lock. Heap/PSRAM (#82) and the last-boot reset reason (#84) are system-global and read
	// directly by the HTTP layer.
	struct Telemetry
	{
		bool     anchorConnected  = false;   // upstream control link (WS) up
		bool     mediaActive      = false;   // a trunk call is bridged right now
		int      tlsSocketsEst    = 0;       // est. anchor TLS sockets (3 persistent + 2/call)
		uint64_t playoutUnderruns = 0;       // current MediaBridge PlayoutBuffer (#81)
		uint64_t playoutOverruns  = 0;
		size_t   clientsUsed      = 0;       // registered extensions
		size_t   clientsCap       = 0;       // POCKETDIAL_MAX_CLIENTS
		size_t   sessionsUsed     = 0;       // active dialogs
		size_t   sessionsCap      = 0;       // POCKETDIAL_MAX_SESSIONS
		uint32_t tlsFullHandshakes    = 0;   // media-stream opens that paid a full TLS handshake
		uint32_t tlsResumedHandshakes = 0;   // ...that resumed a cached session (the fast path)
	};
	Telemetry getTelemetry();

	// Call Detail Records (CDR): a thread-safe snapshot of the recent-call ring,
	// newest first. Copied out under _snapshotMutex like the client/session views.
	std::vector<CallDetailRecord> getCallDetailRecords();

	// Do Not Disturb (DND): set/query a per-extension flag. setDnd is the mutating
	// path behind POST /api/dnd (thread-safe; takes _mutex). getDndExtensions
	// returns the set of extensions currently in DND from the dashboard snapshot
	// (thread-safe; takes _snapshotMutex). Both are safe to call off the SIP thread.
	void setDnd(const std::string& extension, bool on);
	std::vector<std::string> getDndExtensions();

	// Call forwarding (CFU/CFB/CFNA). setForward mutates one trigger ("always",
	// "busy" or "noanswer") for an extension; an empty target clears it (and the
	// whole entry once all three are empty). Both are thread-safe (take _mutex /
	// _snapshotMutex) and NVS-persisted, mirroring setDnd/getDndExtensions. The
	// getter returns {extension, always, busy, noAnswer} tuples for the dashboard.
	void setForward(const std::string& extension, const std::string& trigger, const std::string& target);
	std::vector<std::tuple<std::string, std::string, std::string, std::string>> getForwards();

	// Call parking (park-orbit). Thread-safe snapshot of every occupied orbit slot
	// for the dashboard/TUI: {orbit ext ("700".."70N"), parked extension, parker
	// (the ring-back target on timeout), seconds parked}. Mirrors the other
	// snapshot getters (takes _snapshotMutex only).
	std::vector<std::tuple<std::string, std::string, std::string, int>> getParkedCalls();
	// Override the parked-call timeout (default POCKETDIAL_PARK_TIMEOUT_SEC).
	// Admin/test hook; thread-safe (takes _mutex).
	void setParkTimeout(std::chrono::seconds t);

	// Ring/hunt groups. setRingGroup replaces a group's membership + mode; an empty
	// member list deletes the group. Thread-safe and NVS-persisted. The getter
	// returns {groupExt, "ringall"|"hunt", "m1,m2,..."} for the dashboard.
	void setRingGroup(const std::string& groupExt, const std::string& members, const std::string& mode);
	std::vector<std::tuple<std::string, std::string, std::string>> getRingGroups();

	// Paging zones (980–989). setPageZone replaces a zone's membership (deduped and
	// clamped to POCKETDIAL_ZONE_MEMBER_CAP); an empty member list deletes the zone.
	// Thread-safe and NVS-persisted, mirroring setRingGroup. The getter returns
	// {zoneExt, "m1,m2,..."} pairs for the dashboard/TUI.
	void setPageZone(const std::string& zoneExt, const std::string& members);
	std::vector<std::pair<std::string, std::string>> getPageZones();

	// ── Admin extension (Task 2B) ─────────────────────────────────────────────────
	// NVS-persisted extension identity for the administrative endpoint.
	// Default "101". Loaded from NVS namespace "pbxcfg", key "admin_ext" at boot.
	std::string getAdminExt() const;

	// ── WAN trunk (3CX call-control API) config ──────────────────────────────────
	// NVS-persisted ("storage"/"3cx_*") credentials for the PSTN trunk. The dial
	// plan exposes it as the dial-9 prefix: "9<number>" goes out the trunk. The
	// setter validates + persists; the new client config takes effect on reboot
	// (the live anchor client is not restarted mid-session). Thread-safe.
	struct TrunkConfig
	{
		std::string baseUrl;       // https://pbx.example.com:5001
		std::string clientId;      // 3CX API client id
		std::string clientSecret;  // 3CX API client secret (never display)
		std::string sourceDn;      // the DN the device originates calls as
		bool useLoopback = true;   // true = mock loopback anchor (no PSTN)
	};
	TrunkConfig getTrunkConfig();
	std::string setTrunkConfig(const TrunkConfig& cfg);  // "" on success
	bool isTrunkConnected();   // live anchor state (for the TRUNK status chip)

	// ── Telephony APIs (per-provider credential slots) ────────────────────────────
	// Bounded table of provider credentials (TelephonyApiConfig, NVS namespace
	// "tapicfg" / 0600 host file). Secrets are write-only: the getters return
	// display-safe SlotViews (`secretSet` only — the value never crosses).
	// Changes take effect on the next reboot (mirrors setTrunkConfig). Thread-safe.
	std::vector<TelephonyApiConfig::SlotView> getTelephonyApis();
	// `keepSecret`=true retains the stored secret (UI sends empty for "unchanged").
	// All return "" on success, else a short operator-facing error.
	std::string setTelephonyApi(size_t idx, const TelephonyApiConfig::Slot& s, bool keepSecret);
	std::string clearTelephonyApi(size_t idx);   // zeroize + persist
	// idx == TelephonyApiConfig::kNoActiveSlot clears the selection (legacy trunk
	// config then drives provider choice, exactly as before this table existed).
	std::string setTelephonyApiActive(size_t idx);

	// ── Registrar mode (STAGE 2) ──────────────────────────────────────────────────
	// Runtime registrar policy, replacing the compile-time POCKETDIAL_OPEN_REGISTRAR
	// gate. NVS-persisted (namespace "pbxcfg", key "reg_mode"); the compile-time
	// symbol now only seeds the DEFAULT at boot. Thread-safe (mirror setDnd): setter
	// takes _mutex + refreshes the snapshot; getter reads an atomic so the SIP hot
	// path (onRegister) never locks just to branch on the mode.
	enum class RegistrarMode : uint8_t
	{
		Open   = 0,   // standalone: accept every REGISTER, no challenge (legacy)
		Learn  = 1,   // TOFU + MAC-lock: adopt unknown devices, enforce secured ones
		Secure = 2,   // require digest auth for every provisioned extension
	};
	void setRegistrarMode(RegistrarMode mode);
	RegistrarMode getRegistrarMode() const;

	// #107: anchor TLS re-warm cadence (MINUTES); 0 = disabled. Persists to NVS and pushes
	// the new cadence (as seconds) into the live anchor immediately. getter is lock-free.
	void setRewarmMinutes(uint16_t minutes);
	uint16_t getRewarmMinutes() const;

	// ── Device registry (STAGE 2: Learn-mode adoption) ────────────────────────────
	// Adopted-device lifecycle for the TUI. A device is keyed by its 12-hex MAC and
	// remembers the extension it registered as and whether it has been promoted from
	// first-seen (Learned) to digest-enforced (Secured). All accessors are
	// thread-safe (snapshot mutex) and NVS-persisted, mirroring forwards/groups.
	enum class DeviceState : uint8_t
	{
		Learned = 0,   // TOFU: seen + accepted, not yet locked to digest auth
		Secured = 1,   // promoted: MAC-locked + digest-enforced for its extension
	};
	struct AdoptedDevice
	{
		std::string mac;         // 12 lowercase hex chars
		std::string extension;   // the AOR it last registered as
		DeviceState state = DeviceState::Learned;
		bool online = false;     // currently has a live registration binding
	};
	// Snapshot of all adopted devices for the dashboard/TUI (thread-safe).
	std::vector<AdoptedDevice> getAdoptedDevices();
	// Promote a device to Secured (MAC-locked + digest-enforced). Accepts either a
	// 12-hex MAC or an extension (resolved to the device currently bound to it).
	// Returns false if no such device is known. Thread-safe + persisted.
	bool secureDevice(const std::string& macOrExt);
	// Forget a device entirely (drops the adoption record; a later REGISTER re-learns
	// it in Learn mode). Accepts a MAC or an extension. Thread-safe + persisted.
	bool forgetDevice(const std::string& macOrExt);

private:
	void initHandlers();

	// SIP request handlers (camelCase to match C++ convention)
	void onRegister(std::shared_ptr<SipMessage> data);
	void onOptions(std::shared_ptr<SipMessage> data);
	void onCancel(std::shared_ptr<SipMessage> data);
	void onReqTerminated(std::shared_ptr<SipMessage> data);
	void onInvite(std::shared_ptr<SipMessage> data);
	// Mid-dialog re-INVITE (RFC 3261 §12.2: To header carries a tag) for an
	// established (Connected/Held) session — the hold/resume path. Relays the
	// re-INVITE UNTOUCHED to the peer leg (no clearBody()/enforceG711(), so the
	// hold SDP and its Content-Length stay intact) and tracks the Held/Connected
	// state from the offered SDP direction. Caller holds _mutex (via handle()).
	void onReinvite(std::shared_ptr<SipMessage> data);
	// Rebuild and publish the dashboard session view from the live _sessions map
	// immediately, without waiting for the next 1 Hz tick(). Caller holds _mutex;
	// this takes _snapshotMutex (the _mutex -> _snapshotMutex order used by
	// tick()/setDnd()). Used so a hold/resume state change surfaces on the
	// dashboard at once.
	void refreshSessionSnapshot();
	void onTrying(std::shared_ptr<SipMessage> data);
	void onRinging(std::shared_ptr<SipMessage> data);
	void onBusy(std::shared_ptr<SipMessage> data);
	void onUnavailable(std::shared_ptr<SipMessage> data);
	void onBye(std::shared_ptr<SipMessage> data);
	void onOk(std::shared_ptr<SipMessage> data);
	void onAck(std::shared_ptr<SipMessage> data);
	void onRefer(std::shared_ptr<SipMessage> data);   // blind transfer (RFC 3515)
	void onMessage(std::shared_ptr<SipMessage> data); // inbound MESSAGE (RFC 3428): ack 200 OK

	// ── BLF/presence: SUBSCRIBE/NOTIFY dialog-event package (RFC 6665 + 4235) ────
	// onSubscribe: Event-package gate (489 on anything but "dialog"), AOR validation
	// (400), fixed-slot allocation (503 on exhaustion), 202 Accepted + an immediate
	// full-state NOTIFY. A refresh is matched by Call-ID; Expires: 0 unsubscribes
	// (terminal NOTIFY, slot freed). Called from handle() — caller holds _mutex.
	void onSubscribe(std::shared_ptr<SipMessage> data);

	// One BLF watcher dialog. Fixed-size record in a std::array — no heap growth.
	struct DialogSubscription
	{
		bool        used = false;
		std::string callId;        // subscription dialog id (refresh/unsubscribe key)
		std::string watcherFrom;   // subscriber's full From header (incl. its tag)
		std::string subTo;         // our full To header (incl. the tag we minted)
		std::string targetAor;     // the extension being watched (the To user-part)
		std::string lastState;     // last NOTIFYed state token (change detection)
		unsigned    version = 0;   // dialog-info version counter (monotonic)
		unsigned    cseq = 1;      // NOTIFY CSeq within the subscription dialog
		int         expiresSec = 0;
		sockaddr_in addr{};        // where NOTIFYs go (the SUBSCRIBE source)
		std::chrono::steady_clock::time_point deadline{};
	};
	std::array<DialogSubscription, POCKETDIAL_MAX_SUBSCRIPTIONS> _subscriptions;

	// Compute the current RFC 4235 dialog state of `targetAor` from the registrar +
	// session tables: ""=idle (no dialog element), else trying/early/confirmed plus
	// the direction and a stable dialog id. Caller holds _mutex.
	std::string computeDialogState(const std::string& targetAor,
		std::string& outDirection, std::string& outDialogId) const;

	// Build one NOTIFY for a subscription slot carrying a dialog-info+xml body.
	// `terminated` selects Subscription-State: terminated;reason=<termReason>
	// (otherwise active;expires=<remaining>). Caller holds _mutex.
	std::shared_ptr<SipMessage> buildDialogNotify(DialogSubscription& sub,
		const std::string& state, const std::string& direction, const std::string& dialogId,
		bool terminated, const char* termReason);

	// Single change-detection pass: recompute every watched target's state and
	// NOTIFY the slots whose state changed since the last pass. Called at the end
	// of handle() and from tick() (inside _mutex; NOTIFYs go into _outbox and are
	// sent after the lock is released) — this one hook covers registration
	// appear/disappear, session creation, state transitions and teardown without
	// scattering per-event hooks through the call paths.
	void refreshSubscriptions();

	// Expire overdue subscriptions: terminal NOTIFY (reason=timeout) + slot free.
	// Called from tick(); caller holds _mutex.
	void sweepSubscriptions();

	// ── DTMF SIP INFO handler (Task 2C) ──────────────────────────────────────────
	// Invoked from handle() when a SIP INFO arrives carrying
	// Content-Type: application/dtmf-relay. Parses the Signal= digit, updates the
	// per-Call-ID accumulator, and dispatches CLASS feature code actions.
	// Called from the single-threaded SIP handler path — no additional mutex needed.
	void onDtmfInfo(std::shared_ptr<SipMessage> data);

	// ── Register beep (signaling-only intercom tone) ─────────────────────────────
	// On a NEW registration, send the registering phone a brief auto-answer INVITE so
	// it plays its own intercom tone (the "beep"), then tear the call straight back
	// down. NO RTP is ever sourced — the tone is the phone's local intercom alert. The
	// outbound UAC dialog is tracked in a small bounded ring (_beepDialogs) keyed by
	// Call-ID so onOk() can drive ACK→BYE and tick() can time it out / CANCEL it. All
	// of these assume the caller already holds _mutex (non-recursive).
	//
	// State machine (per beep dialog):
	//   sendRegisterBeep() : allocate a slot, send INVITE (auto-answer headers), arm
	//                        a deadline; if no slot free, skip the beep (it's cosmetic).
	//   onOk() INVITE 200       : send ACK, then BYE, advance to AwaitingByeOk.
	//   onOk() BYE 200          : free the slot.
	//   tick() deadline (Invite): CANCEL the INVITE and advance to AwaitingCancelDone —
	//                             do NOT free yet (the INVITE transaction isn't done
	//                             until the phone's 487 is received and ACKed; #90).
	//   onReqTerminated() 487   : ACK the final response (RFC 3261 §17.1.1.3), free slot.
	//   tick() deadline (Bye /  : free the slot (BYE was best-effort; or the CANCEL
	//   CancelDone)               linger elapsed without a 487 — bounded, never leaks).
	void sendRegisterBeep(const std::shared_ptr<SipClient>& phone);
	std::shared_ptr<SipMessage> buildBeepAck(const std::shared_ptr<SipMessage>& finalResp);
	std::shared_ptr<SipMessage> buildBeepBye(const std::shared_ptr<SipMessage>& ok);
	std::shared_ptr<SipMessage> buildBeepCancel(std::size_t slot);

	// AwaitingCancelDone: CANCEL sent, lingering until the 487 final response is ACKed
	// (or a bounded deadline frees the slot). Added for #90 — see the teardown notes above.
	enum class BeepState { Free, AwaitingInviteOk, AwaitingByeOk, AwaitingCancelDone };
	struct BeepDialog
	{
		BeepState state = BeepState::Free;
		std::string callID;        // fresh per beep; how onOk()/tick() find this slot
		std::string branch;        // Via branch (reused for INVITE/CANCEL)
		std::string fromTag;       // our (server) From tag
		std::string ext;           // target extension (phone number)
		sockaddr_in addr{};        // phone's contact address
		std::chrono::steady_clock::time_point deadline{};
	};
	// Bounded outbound-UAC dialog table. Tiny fixed footprint; if all slots are busy a
	// new registration just skips its beep. Guarded by _mutex.
	std::array<BeepDialog, POCKETDIAL_MAX_BEEPS> _beepDialogs;
	// Find the beep slot owning a Call-ID, or nullptr. Caller holds _mutex.
	BeepDialog* findBeepByCallID(std::string_view callID);

	bool setCallState(std::string_view callID, Session::State state);
	void endCall(std::string_view callID, std::string_view srcNumber, std::string_view destNumber, std::string_view reason = "");

	// CDR: write one record into the ring as a call ends. Caller must hold _mutex.
	// `session` (may be null) supplies the start time / final state used to derive
	// duration and result; src/dest provide the parties when the session lookup
	// can't (e.g. the virtual 777/999 extensions reuse a shared dummy client).
	void recordCdr(const std::shared_ptr<Session>& session,
		std::string_view srcNumber, std::string_view destNumber);
	uint64_t nowEpochMs() const;

	// Internal DND lookup used by onInvite(). Caller MUST already hold _mutex
	// (std::mutex is non-recursive); does a bounded map lookup, no locking.
	bool isDndEnabled(const std::string& extension);

	// Internal forward/group lookups used by onInvite()/onBusy()/tick(). Caller
	// MUST already hold _mutex (non-recursive) — bounded map lookups, no locking.
	// getForwardTarget returns "" when no forward of that trigger is configured.
	std::string getForwardTarget(const std::string& extension, const std::string& trigger) const;
	const pbx::RingGroup* findRingGroup(const std::string& extension) const;

	// Internal paging-zone lookups. Caller MUST already hold _mutex — bounded map
	// lookups, no locking. findPageZone resolves a configured zone; isPageZoneDialog
	// answers "does this dialog To-number address a configured paging zone?" so
	// onCancel/onBye/onAck can route zone pages through the 999 broadcast paths.
	const pbx::PageZone* findPageZone(const std::string& extension) const;
	bool isPageZoneDialog(const std::string& extension) const;

	// Fan an INVITE out to a set of targets (the reusable core extracted from the
	// 999 all-page path). `targets` are pre-selected registered clients; `intercom`
	// adds the 999 auto-answer headers (true for 999, false for a ring group so it
	// rings normally). Builds the broadcast Session, the 180 Ringing to the caller,
	// and one forked INVITE per target. Caller holds _mutex.
	void startBroadcastFork(std::shared_ptr<SipMessage> invite,
		std::shared_ptr<SipClient> caller,
		const std::vector<std::shared_ptr<SipClient>>& targets,
		bool intercom);

	// Build and queue a single INVITE fork toward one target, re-pointing the
	// request line / To at that target. `intercom` toggles the auto-answer headers.
	// Caller holds _mutex.
	void buildInviteFork(const std::shared_ptr<SipMessage>& invite,
		const std::shared_ptr<SipClient>& caller,
		const std::shared_ptr<SipClient>& target,
		bool intercom);

	// Drive the next leg of a sequential hunt group (ring one member, arm timeout).
	// Returns false when the member list is exhausted. Caller holds _mutex.
	bool huntRingNext(const std::shared_ptr<Session>& session);

	// Re-target an INVITE at `target` and (re)send it as a fresh call leg — the
	// engine behind blind-transfer and call-forward "redirect" paths. Caller holds
	// _mutex. Returns false if the target is not registered.
	bool redirectInvite(const std::shared_ptr<SipMessage>& invite,
		const std::shared_ptr<SipClient>& caller,
		const std::string& target);

	// Build a NOTIFY (Event: refer) carrying a message/sipfrag body reporting the
	// transfer result back to the transferor. Caller holds _mutex.
	std::shared_ptr<SipMessage> buildReferNotify(const std::shared_ptr<SipMessage>& refer,
		const std::shared_ptr<SipClient>& transferor,
		const std::string& sipfrag,
		bool terminated);

	// ── Media beachhead: virtual extension 440 (server-sourced RTP tone) ─────────
	// onInvite() routes a dial of 440 here. The server answers 200 OK advertising its
	// OWN media (server IP:port, m=audio <svrport> RTP/AVP 0, PCMU) and starts the
	// one-way RTP tone stream to the caller's RTP address. ONE concurrent stream: a
	// 2nd dial while busy is rejected 486 Busy Here. Caller holds _mutex.
	void onMediaInvite(std::shared_ptr<SipMessage> data, const std::shared_ptr<SipClient>& caller);

	// ── Call parking / park-orbit (roadmap §3.1 P1) ──────────────────────────────
	// Virtual orbit extensions 700..70(N-1). An INVITE to a FREE orbit parks the
	// caller's leg there (the server answers 777-style — the caller's own SDP is
	// echoed back, no server media); an INVITE to an OCCUPIED orbit retrieves it:
	// the retriever is answered with the parked party's SDP and the parked party is
	// re-INVITEd (in its existing dialog) with the retriever's SDP, so media
	// renegotiates peer-to-peer. The orbit table is a fixed array (pool discipline);
	// each parked call additionally pins ONE Session slot — see PoolConfig.hpp.
	// tick() sweeps timed-out parks: ring back the parker (Referred-By of the
	// parking INVITE) when registered, else tear down with BYE. All helpers assume
	// the caller holds _mutex and only enqueue to _outbox.
	enum class ParkState : uint8_t { Free, Parked, RingingBack, Retrieving };
	struct ParkSlot
	{
		ParkState state = ParkState::Free;
		std::string orbit;              // "700".."70N"
		std::string callID;             // full "Call-ID: ..." line of the parked dialog
		std::string parkedExt;          // the parked party's extension
		sockaddr_in parkedAddr{};       // the parked party's signaling address
		// Issue #69 (L-5): the orbit used to pin the whole pooled SipMessage here for
		// up to PARK_TIMEOUT_SEC, holding a MSG_POOL slot hostage and feeding the M-2
		// exhaustion cliff. We only ever read the parked party's SDP body and their
		// From-tag off it, so copy just those two minimal dialog fields out and let the
		// pooled message return to the pool immediately. `parked` (set true on park)
		// replaces the old "is slot.invite non-null?" validity guard.
		bool        parked = false;     // slot has a captured parked dialog
		std::string parkedSdp;          // parked party's SDP (for retrieve / ring-back offers)
		std::string parkedFromTag;      // parked party's From-tag (re-INVITE / BYE To-tag)
		std::string localTag;           // our UAS To-tag on the parked dialog
		std::string parker;             // ring-back target on timeout
		std::chrono::steady_clock::time_point parkedAt{};
		// Ring-back UAC dialog toward the parker (server-minted, fresh Call-ID).
		std::string rbCallID;           // full "Call-ID: ..." line
		std::string rbFromTag;
		std::string rbBranch;
		sockaddr_in rbAddr{};
		std::chrono::steady_clock::time_point deadline{};   // ring-back / re-INVITE guard
	};
	std::array<ParkSlot, POCKETDIAL_PARK_SLOTS> _parkSlots;
	std::chrono::seconds _parkTimeout{POCKETDIAL_PARK_TIMEOUT_SEC};
	// Call-IDs of parked dialogs we have sent an in-dialog re-INVITE to (retrieve /
	// ring-back media re-point) and still owe an ACK for, once the parked party
	// 200s it. Bounded by the orbit table (one entry per in-flight retrieve).
	std::vector<std::string> _parkPendingAcks;

	// Orbit index for an AOR ("700".."70N" → 0..N-1), or -1 if not an orbit.
	int parkOrbitIndex(std::string_view ext) const;
	// INVITE routed at an orbit extension: park (free slot) or retrieve (occupied).
	void onParkInvite(std::shared_ptr<SipMessage> data,
		const std::shared_ptr<SipClient>& caller, int orbitIdx);
	// In-dialog re-INVITE to the parked party carrying `sdp` (the retriever's or
	// ring-back answerer's offer) so the two phones renegotiate media P2P.
	void sendParkReinvite(ParkSlot& slot, const std::string& sdp);
	// Server-as-UAS BYE tearing down the parked dialog (timeout / failure paths).
	void byeParkedParty(const ParkSlot& slot);
	// Server-as-UAC INVITE ringing the parker back after a park timeout.
	void startParkRingback(ParkSlot& slot, const std::shared_ptr<SipClient>& parker,
		std::chrono::steady_clock::time_point now);
	// 200 OK hook (called from onOk before the session lookup, like the beep table).
	// Returns true when the OK belonged to a park dialog and was consumed.
	bool handleParkOk(const std::shared_ptr<SipMessage>& data);
	// Timed-out park sweep, polled from tick().
	void parkSweep(std::chrono::steady_clock::time_point now);
	// Free any slot owned by `callID` (parked or ring-back leg); endCall() hook.
	void freeParkSlot(std::string_view callID);
	// Mirror the orbit table into the dashboard snapshot (caller holds _mutex).
	void refreshParkSnapshot();

	bool registerClient(std::shared_ptr<SipClient> client);
	void unregisterClient(std::string_view number);

	// Registration-lease handling (RFC 3261 §10.2.1)
	int parseRequestedExpires(const std::shared_ptr<SipMessage>& data) const;
	void sweepExpired();   // evict expired bindings; caller must hold _mutex
	void maybeSweep();     // throttled sweep; caller must hold _mutex

	std::optional<std::shared_ptr<SipClient>> findClient(std::string_view number);
	std::optional<std::shared_ptr<SipClient>> findClientByAddress(const sockaddr_in& addr);
	std::shared_ptr<SipMessage> buildOptionsPing(const std::shared_ptr<SipClient>& client);

	// ── Outbound SIP MESSAGE (STAGE 2) ────────────────────────────────────────────
	// Originate a one-shot SIP MESSAGE (RFC 3428, Content-Type text/plain) to a
	// registered extension — e.g. to notify a phone/operator of a freshly assigned
	// secret. Mirrors the register-beep UAC enqueue (build + _outbox), best-effort:
	// returns false (no enqueue) if `ext` is not currently registered. The body is
	// length-bounded to keep the message in a single UDP datagram. Thread-safe: takes
	// _mutex. Safe to call off the SIP thread (the TUI/admin path).
	bool sendMessageTo(const std::string& ext, const std::string& text);

	// Broadcast / all-page extension (Issue #37). All assume the caller holds _mutex.
	void startPaging(std::shared_ptr<SipMessage> invite, std::shared_ptr<SipClient> caller);
	void handlePagingAnswer(const std::shared_ptr<Session>& session, std::shared_ptr<SipMessage> data);
	std::shared_ptr<SipMessage> buildCancel(const std::shared_ptr<SipMessage>& invite,
		const std::shared_ptr<SipClient>& target);
	std::shared_ptr<SipMessage> buildPagingBye(const std::shared_ptr<SipMessage>& ok,
		const std::shared_ptr<SipClient>& answerer);

	// Issue #38: per-source-IP token bucket + optional allowlist. Both helpers
	// assume the caller already holds _mutex.
	bool ipAllowed(const sockaddr_in& src) const;
	bool allowPacket(const sockaddr_in& src);

	void endHandle(std::string_view destNumber, std::shared_ptr<SipMessage> message);
	std::string buildContact(std::string_view number) const;

	bool isValidAor(std::string_view s) const;
	void queueLog(std::string msg, bool isError = false);

	std::shared_ptr<SipClient> allocateClient(std::string number, sockaddr_in address, int expiresSeconds);
	std::shared_ptr<Session> allocateSession(std::string callID, std::shared_ptr<SipClient> src);
	// Issue #70 (L-6): draw a transient virtual-peer SipClient (777/440/park/PSTN leg)
	// from the fixed _virtualPeerPool instead of make_shared'ing one in the packet
	// handler. A slot is free when only the pool holds it (use_count()==1); on
	// exhaustion it falls back to a one-off heap SipClient (graceful, never a crash).
	// Caller holds _mutex (the pool scan/reset is not otherwise serialised).
	std::shared_ptr<SipClient> allocateVirtualPeer(std::string number, sockaddr_in address, int expiresSeconds = 3600);
	void dumpWire(const char* label, const std::shared_ptr<SipMessage>& msg);
	void dumpWire(const char* label, std::string_view method, std::string_view callID, std::string_view to);
	std::shared_ptr<SipMessage> buildOkWithSdp(
		const std::shared_ptr<SipMessage>& inviteMsg,
		const std::string& activeIp,
		const std::string& toTag,
		const std::string& sdpBody);
	// The single server-initiated in-dialog BYE builder (Issue #11). Callers supply
	// the complete From/To headers — including tags — because the dialog roles
	// differ: the beep dialog is server-as-UAC (From = our INVITE tag), the anchor
	// dialog is server-as-UAS (From = the To-tag we minted on the 180/200).
	std::shared_ptr<SipMessage> buildServerBye(
		const std::string& destExt,
		const sockaddr_in& destAddr,
		const std::string& callId,
		const std::string& fromHeader,
		const std::string& toHeader);
	void sendRinging(
		const std::shared_ptr<SipMessage>& data,
		const std::string& activeIp,
		const std::string& toTag,
		const char* logPrefix);
	// Route an INVITE out the WAN anchor (3CX/loopback). `dialed` is the number
	// handed to the trunk — the 9-prefix is already stripped by the dial plan.
	void routeAnchorCall(const std::shared_ptr<SipMessage>& data,
	                     const std::shared_ptr<SipClient>& caller,
	                     const std::string& dialed);
	void asyncMakeCall(const std::string& destination, const std::string& callId, const std::string& callerNumber);
	void asyncDropCall(const std::string& participantId);
	void asyncAnswerCall(const std::string& participantId);

	// ── Inbound anchor (PSTN → local extension) ──────────────────────────────────
	// The mirror of routeAnchorCall: an upstream CallEvent::Incoming for a monitored
	// DN makes the server a UAC that originates an INVITE *toward* the local handset
	// registered as that DN. Unlike the outbound (server-as-UAS) leg these dialogs are
	// keyed by a server-minted Call-ID and carry their UAC state on the Session
	// (isAnchorInbound/UacBranch/RemoteTag/AnchorParticipantId).
	//   routeInboundAnchorCall : RING-ALL — fork a delayed-offer INVITE to every
	//                            registered extension (the monitored DN is a 3CX route
	//                            point, not a phone), arm a no-answer timer. First answer
	//                            wins; the rest are CANCELled. Drops the upstream leg if no
	//                            extension is registered or the one media bridge is busy.
	//   onInboundAnchorOk      : a handset's 200 OK — the FIRST resolves the winner (by
	//                            source address), starts the media bridge to its RTP, ACKs
	//                            with our SDP answer, async answerCall()s the upstream leg,
	//                            and CANCELs the other ringing legs. A later answer from a
	//                            losing leg (crossed our CANCEL) is ACKed then BYEd.
	// Both run under _mutex (the event callback / SIP handler already hold it).
	void routeInboundAnchorCall(const std::string& participantId, const std::string& callerId);
	void onInboundAnchorOk(const std::shared_ptr<SipMessage>& ok, const std::shared_ptr<Session>& session);
	// One delayed-offer INVITE toward `target`, reusing the session's shared Call-ID / Via
	// branch / From-tag so every forked leg is one cancellable transaction set. Emits on
	// _asyncOutbox (the anchor event callback runs off the SIP thread). Caller holds _mutex.
	void buildInboundInviteFork(const std::shared_ptr<Session>& session,
	                            const std::shared_ptr<SipClient>& target,
	                            const std::string& callerDisplay);
	// CANCEL our outstanding INVITE toward a still-ringing forked leg (`target`), built from
	// the Session's shared UAC dialog state. Used to stop the losers when one extension
	// answers, and to abort all legs on no-answer timeout / PSTN-caller abandon. Caller
	// holds _mutex.
	std::shared_ptr<SipMessage> buildInboundCancelTo(const std::shared_ptr<Session>& session,
	                                                 const std::shared_ptr<SipClient>& target);
	// Back-compat single-leg wrapper: CANCEL the session's current dest. Caller holds _mutex.
	std::shared_ptr<SipMessage> buildInboundCancel(const std::shared_ptr<Session>& session);
	// ACK a non-2xx final (480/486/487/…) from one forked inbound leg. RFC 3261 §17.1.1.3:
	// the server is the UAC, so it MUST ACK the failure within the INVITE transaction — the
	// ACK reuses the shared fork Via branch + the response's To (carries the leg's tag). No
	// other party relays it (there is no caller phone). Caller holds _mutex; emits on _outbox.
	void ackInboundFinal(const std::shared_ptr<Session>& session, const std::shared_ptr<SipMessage>& data);


	// Server-side RTP media source (the 440 tone stream). One concurrent stream; the
	// ESP-only UDP socket + 20 ms pacing task live inside it, guarded for host builds.
	RtpSender            _rtpSender;
	RtpReceiver          _rtpReceiver;
	ThreeCxAnchorClient  _threeCxClient;
	LoopbackAnchorClient _loopbackClient;
	// Honest stubs for declared-but-unimplemented providers (compile-time
	// scaffolding only: start() fails, isConnected() is always false).
	StubTelephonyProvider _stubApidaze{TelephonyProviderType::Apidaze};
	StubTelephonyProvider _stubVoipInnovations{TelephonyProviderType::VoipInnovations};
	StubTelephonyProvider _stubSangoma{TelephonyProviderType::Sangoma};
	// Fixed-size type→instance factory; populated once in loadAnchorConfig().
	TelephonyProviderRegistry _providerRegistry;
	// Telephony-API credential slots (guarded by _mutex, like _trunkCfg).
	TelephonyApiConfig   _tapiCfg;
	AnchorClient*        _anchorClient = nullptr;
	MediaBridge          _mediaBridge;

#if !defined(ESP_PLATFORM) && !defined(ESP32) && !defined(ARDUINO)
	// Async anchor-start worker (host builds). Must be JOINED in the destructor:
	// the previous detached thread captured `this` and outlived the handler in
	// unit tests, segfaulting the suite nondeterministically.
	std::thread _anchorStartThread;
	// asyncMakeCall/asyncDropCall/asyncAnswerCall workers (host builds). Same lifetime
	// rule as _anchorStartThread: they capture `this`, so they must be joined — never
	// detached. Issue #67: previously these accumulated in a bare vector<thread> joined
	// only in ~RequestsHandler, so a long-lived host/CI session leaked one joinable
	// thread object per call action. Each worker now flips a `done` flag as its last
	// act; reapAnchorWorkers() (called from tick()) joins and erases the finished ones,
	// bounding the live count to in-flight calls instead of total-calls-ever.
	struct AnchorWorker
	{
		std::thread thread;
		std::shared_ptr<std::atomic<bool>> done;
	};
	std::mutex _anchorWorkMutex;
	std::vector<AnchorWorker> _anchorWorkThreads;
	// Spawn a detach-free anchor worker: runs `job`, then sets its done-flag. Pushes
	// it onto _anchorWorkThreads under _anchorWorkMutex. Host-only.
	void spawnAnchorWorker(std::function<void()> job);
	// Join + erase any workers that have finished. Called from tick(); also drains all
	// remaining workers when `drainAll` is true (used by the destructor). Host-only.
	void reapAnchorWorkers(bool drainAll = false);
	// Join + erase finished workers. Caller MUST hold _anchorWorkMutex. Host-only.
	void reapFinishedLocked();
#endif

	// RequestsHandler.hpp: Issues #24 and #28 resolved.
	std::unordered_map<std::string, std::function<void(std::shared_ptr<SipMessage> request)>> _handlers;
	std::unordered_map<std::string, std::shared_ptr<Session>>   _sessions;

	std::mutex _mutex;
	OnHandledEvent _onHandled;
	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> _outbox;

	// Out-of-band send queue for messages enqueued OFF the SIP receive thread —
	// e.g. the 3CX WebSocket Answered/Dropped callbacks (200 OK, BYE). handle() and
	// tick() both clear _outbox at the START of their body (per-pass scratch), which
	// silently wiped async sends before they ever reached the wire. _asyncOutbox is
	// only ever appended off-thread and drained at the END of handle()/tick(); it is
	// never cleared at the start, so an async 200 OK survives an intervening inbound
	// INVITE retransmit. Guarded by _mutex like _outbox.
	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> _asyncOutbox;

	std::string _serverIp;
	int         _serverPort;

	std::atomic<uint64_t> _packetsProcessed{0};
	std::atomic<uint64_t> _packetsDropped{0};

	struct RegistrarSnapshot
	{
		std::vector<std::pair<std::string, std::string>> clients;
		std::vector<std::tuple<std::string, std::string, std::string, int>> sessions;
		std::vector<CallDetailRecord> cdr;   // newest first
		std::vector<std::string> dnd;        // extensions currently in DND
		// Call-forward config: {extension, always, busy, noAnswer}.
		std::vector<std::tuple<std::string, std::string, std::string, std::string>> forwards;
		// Ring/hunt groups: {groupExt, "ringall"|"hunt", "m1,m2,..."}.
		std::vector<std::tuple<std::string, std::string, std::string>> ringGroups;
		// Paging zones: {zoneExt, "m1,m2,..."}.
		std::vector<std::pair<std::string, std::string>> pageZones;
		// Adopted devices (STAGE 2): {mac, ext, state, online}. Mirrored from _devices
		// under _mutex; copied out for the TUI under _snapshotMutex.
		std::vector<AdoptedDevice> devices;
		// Parked calls: {orbit, parkedExt, parker, secondsParked}.
		std::vector<std::tuple<std::string, std::string, std::string, int>> parked;
		uint64_t packetsProcessed = 0;
		uint64_t packetsDropped = 0;
	};
	RegistrarSnapshot _snapshot;
	std::mutex _snapshotMutex;

	// CDR ring buffer (Phase 2). Fixed capacity, no heap growth: writes wrap and
	// overwrite the oldest slot. All access is under _mutex. _cdrHead is the index
	// of the NEXT slot to write; _cdrCount caps at POCKETDIAL_CDR_RECORDS.
	std::array<CallDetailRecord, POCKETDIAL_CDR_RECORDS> _cdrRing;
	size_t _cdrHead = 0;
	size_t _cdrCount = 0;

	// DND state, keyed by extension. Bounded by the client-pool depth: an entry is
	// only created when DND is turned ON, and turning it OFF erases the entry, so
	// the map can never hold more than POCKETDIAL_MAX_CLIENTS live extensions.
	// Guarded by _mutex. (A std::shared_ptr<SipClient> flag would be lost across
	// re-REGISTER / pool eviction; keying by extension keeps DND sticky.)
	std::unordered_map<std::string, bool> _dnd;

	// Call-forwarding config, keyed by extension (Class A sweep). Same bounding /
	// stickiness rationale as _dnd: an entry exists only while at least one trigger
	// is set, and is bounded by POCKETDIAL_MAX_CLIENTS. Guarded by _mutex; mirrored
	// into the dashboard snapshot and persisted to NVS.
	std::unordered_map<std::string, pbx::ForwardConfig> _forwards;

	// Ring/hunt groups, keyed by the group extension (e.g. 6xx). Bounded by
	// POCKETDIAL_MAX_CLIENTS groups; each member list is bounded by splitMembers().
	// Guarded by _mutex; mirrored into the snapshot and persisted to NVS.
	std::unordered_map<std::string, pbx::RingGroup> _ringGroups;

	// Paging zones, keyed by the zone extension (980–989). Bounded by
	// POCKETDIAL_MAX_PAGE_ZONES; each member list is deduped + clamped to
	// POCKETDIAL_ZONE_MEMBER_CAP by splitZoneMembers(). Guarded by _mutex;
	// mirrored into the snapshot and persisted to NVS ("pbxcfg"/"pzones").
	std::unordered_map<std::string, pbx::PageZone> _pageZones;

	// ── Registrar mode (STAGE 2) ──────────────────────────────────────────────────
	// Atomic so onRegister() can read the policy without taking _mutex (it already
	// holds _mutex via handle(), but keeping this atomic also lets getRegistrarMode()
	// be lock-free for the dashboard). Seeded from POCKETDIAL_OPEN_REGISTRAR at boot,
	// then overridden by the persisted NVS value if present.
#ifdef POCKETDIAL_OPEN_REGISTRAR
	std::atomic<RegistrarMode> _registrarMode{RegistrarMode::Open};
#else
	std::atomic<RegistrarMode> _registrarMode{RegistrarMode::Secure};
#endif
	void loadRegistrarMode();             // boot-time reload from NVS; caller holds _mutex
	void persistRegistrarMode();          // write-through after a mode change; holds _mutex

	// ── #107: anchor TLS re-warm cadence (minutes) ──────────────────────────────────
	// PBX-wide cadence at which the WAN anchor re-warms its IDLE media TLS session so even the
	// first call after a long idle still resumes (vs. a ~1 s software-ECDHE cold handshake).
	// 0 = disabled. Persisted to NVS "pbxcfg" (key "rwm_min"); pushed to the live anchor (as
	// seconds) at boot and on change. Atomic so the dashboard/TUI getter is lock-free. Default
	// 60 min. One PBX-wide value today (single active anchor); per-provider when #100 lands.
	std::atomic<uint16_t> _rewarmMinutes{60};
	void loadRewarmInterval();            // boot-time reload from NVS (ctor; single-threaded)
	void persistRewarmInterval();         // write-through after a change; caller holds _mutex

	// ── Device registry (STAGE 2: Learn-mode adoption) ────────────────────────────
	// Adopted devices keyed by 12-hex MAC. Bounded by POCKETDIAL_MAX_CLIENTS (a flood
	// of distinct MACs cannot grow the heap without limit; new keys past the cap are
	// dropped, mirroring _dnd/_forwards). Guarded by _mutex; mirrored into the
	// snapshot and persisted to NVS (namespace "pbxcfg", key "devices").
	struct DeviceRecord
	{
		std::string extension;
		DeviceState state = DeviceState::Learned;
	};
	std::unordered_map<std::string, DeviceRecord> _devices;   // mac -> record
	void loadDevices();                   // boot-time reload; caller holds _mutex
	void persistDevices();                // mark devices dirty (Issue #55); holds _mutex
	void refreshDeviceSnapshot();         // mirror _devices into _snapshot; holds both mutexes

	// Learn-mode REGISTER admission. Caller holds _mutex. Resolves the source MAC,
	// applies TOFU + MAC-lock, and returns the digest decision. On a first-packet
	// ARP miss it returns Accept (deferring the lock to the next REGISTER). May set
	// `outRejectReason` when returning Reject. Records/updates the adoption entry.
	enum class AuthDecision { Accept, Challenge, Reject };
	AuthDecision admitLearn(const std::shared_ptr<SipMessage>& data,
		const std::string& ext, std::string& outRejectReason);
	// Secure-mode REGISTER admission: challenge + verify digest for `ext`. Caller
	// holds _mutex. Enqueues the 401 (with WWW-Authenticate) itself when challenging.
	AuthDecision admitSecure(const std::shared_ptr<SipMessage>& data,
		const std::string& ext, std::string& outRejectReason);
	// Emit a 401 Unauthorized with a fresh WWW-Authenticate challenge for `data`
	// into _outbox. `stale` answers an expired-but-valid nonce. Caller holds _mutex.
	void sendChallenge(const std::shared_ptr<SipMessage>& data, bool stale);
	// Emit a 403 Forbidden with a reason phrase into _outbox. Caller holds _mutex.
	void sendForbidden(const std::shared_ptr<SipMessage>& data, const std::string& reason);
	// Dialog-source binding for in-dialog teardown (BYE/CANCEL). Returns true iff the
	// UDP packet source IP of `source` matches one of the session's real phone legs
	// (getSrc()/getDest()) — i.e. the request plausibly came from a party in the call.
	// Used to reject off-path teardown spoofing (issue #46 / THREAT_MODEL S-3/D-2):
	// a peer that merely sniffed/guessed a Call-ID but sits at a DIFFERENT IP cannot
	// forge a BYE/CANCEL to drop the call. IP-only (port-agnostic): a phone may emit
	// the teardown from the same contact IP but a different ephemeral port than the
	// INVITE. Returns true (fail-open) when a leg is virtual/server-originated or
	// absent (anchor, 777/999/440, park, half-set-up dialog) — those teardown paths
	// are owned by the dedicated per-extension handlers and must not be broken here.
	// Pure in-memory; no I/O. Caller holds _mutex.
	bool isDialogSourceAuthorized(const std::shared_ptr<Session>& session,
		const sockaddr_in& source) const;
	// Mark a device record online/offline in the snapshot after a (de)registration.
	// Caller holds _mutex.
	void markDeviceOnline(const std::string& mac, bool online);

	// NVS persistence for _forwards / _ringGroups. No-ops on host (the maps are the
	// store); on ESP they read/write the "pbxcfg" NVS namespace. Caller holds _mutex.
	void loadPbxConfig();                 // boot-time reload into the maps
	void loadThreeCxConfig();             // boot-time reload of 3CX settings
	TrunkConfig _trunkCfg;                // last-loaded/saved trunk config (under _mutex)
	// Issue #42/#55: these no longer touch NVS inline. Called under _mutex from the
	// signaling/star-code paths, they only mark the corresponding store dirty (a bit
	// in _nvsDirty). The blocking nvs_open/commit happens later in flushDirtyNvs(),
	// run AFTER the registrar lock is released (debounced write-back) — restoring the
	// "no blocking flash I/O under _mutex" invariant. Caller holds _mutex.
	void persistForwards();               // mark forwards dirty (setForward mutation)
	void persistRingGroups();             // mark ring groups dirty (setRingGroup mutation)
	void persistPageZones();              // mark page zones dirty (setPageZone mutation)
	bool _pbxConfigLoaded = false;

	// Persistent CDR (Class A sweep). The CDR ring is flushed to the "cdrlog" NVS
	// namespace (write-back) and reloaded on boot, so records survive reboot. No-ops
	// on host. Caller holds _mutex.
	void loadCdrRing();                   // boot-time reload of the ring
	void persistCdrRing();                // mark the CDR ring dirty (flushed off-lock)

	// ── Debounced NVS write-back (Issue #42 / #55) ───────────────────────────────
	// The persistXxx() mutators above run under _mutex and must NOT block on flash.
	// Each instead sets a bit in _nvsDirty (guarded by _mutex). flushDirtyNvs() runs
	// from tick() AFTER the registrar lock is dropped: it snapshots+clears the dirty
	// bits and serializes each pending store under a SHORT lock (into a local blob),
	// then performs the blocking nvs_open/set/commit OUTSIDE any lock. No-op on host
	// (the in-memory maps are the store). Coalesces repeated dirtying between ticks
	// into a single flash write, also cutting wear under call/config churn.
	enum NvsDirtyBit : uint8_t {
		kNvsDirtyForwards   = 1u << 0,
		kNvsDirtyRingGroups = 1u << 1,
		kNvsDirtyPageZones  = 1u << 2,
		kNvsDirtyDevices    = 1u << 3,
		kNvsDirtyCdr        = 1u << 4,
	};
	uint8_t _nvsDirty = 0;                 // pending-write bitmask; guarded by _mutex
	void flushDirtyNvs();                  // off-lock; called from tick()
	// Blob serializers — read the in-memory store; the caller holds _mutex while
	// calling them (flushDirtyNvs takes a short lock around each).
	std::string serializeForwardsBlob() const;
	std::string serializeRingGroupsBlob() const;
	std::string serializePageZonesBlob() const;
	std::string serializeDevicesBlob() const;
	std::string serializeCdrBlob() const;

	// Pre-allocated static memory pools (Issue #53)
	std::vector<std::shared_ptr<SipClient>> _clientPool;
	// Fixed pool of transient virtual-peer SipClients (Issue #70). Distinct from
	// _clientPool: these are NOT registered endpoints and never appear in registrar
	// scans — they only back a Session's _dest for the call's lifetime.
	std::vector<std::shared_ptr<SipClient>> _virtualPeerPool;
	std::vector<std::shared_ptr<Session>> _sessionPool;
	static std::vector<std::shared_ptr<SipMessage>> _messagePool;
	// Issue #41: dedicated leaf mutex guarding the static _messagePool. The pool is
	// drawn lock-free from the UDP RX thread (SipMessageFactory::createMessage, before
	// handle() takes _mutex) AND under _mutex from tick()/handler threads, so _mutex
	// does NOT serialise the two. getMessageFromPool() does a check-then-act
	// (use_count()==1 → reset) that is a data race without its own lock. This leaf
	// mutex is held only for the brief slot scan/reset — never around blocking I/O —
	// so it cannot deadlock against _mutex (always acquired innermost, never nested
	// the other way). Ordered AFTER _messagePool so it is constructed/destroyed last.
	static std::mutex _messagePoolMutex;

	// Issue #38: token bucket keyed by source IPv4 (network-order s_addr).
	struct RateBucket
	{
		double tokens;
		std::chrono::steady_clock::time_point last;
	};
	std::unordered_map<uint32_t, RateBucket> _rateBuckets;
	// Optional CIDR allowlist (host order). _allowMask == 0 means "no allowlist".
	uint32_t _allowNet  = 0;
	uint32_t _allowMask = 0;

	std::chrono::steady_clock::time_point _lastSweep{};
	std::chrono::steady_clock::time_point _lastTick{};

	std::vector<std::pair<bool, std::string>> _logQueue;

	// ── Admin extension (Task 2B) ─────────────────────────────────────────────────
	// NVS-persisted extension identity for the administrative endpoint.
	// Default "101". Loaded from NVS namespace "pbxcfg", key "admin_ext" at boot.
	std::string _adminExt{"101"};
	void loadAdminExt();
	void saveAdminExt(const std::string& ext);

	// ── DTMF digit-collection state machine (Task 2C) ────────────────────────────
	// Per-Call-ID accumulator. Accessed only from the single-threaded UDP receiver
	// task (the same path that calls handle()), so no additional mutex is needed.
	struct DtmfAccum
	{
		std::string digits;          // accumulated digit string
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
		TickType_t  lastTick{0};     // xTaskGetTickCount() of last digit
#else
		uint32_t    lastTick{0};     // monotonic ms counter on host
#endif
		static constexpr uint32_t TIMEOUT_MS = 5000;
	};
	std::unordered_map<std::string, DtmfAccum> _dtmfState;
};

#endif
