#ifndef REQUESTS_HANDLER_HPP
#define REQUESTS_HANDLER_HPP

// Gated open mode (Issue #56). Undefine/disable to run in closed/restricted mode.
#define POCKETDIAL_OPEN_REGISTRAR

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include <lwip/sockets.h>
#elif defined(__linux__)
#include <netinet/in.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#endif

#include <functional>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <vector>
#include <tuple>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <array>
#include "SipMessage.hpp"
#include "SipClient.hpp"
#include "Session.hpp"
#include "CallDetailRecord.hpp"
#include "PbxConfig.hpp"

class RequestsHandler
{
public:

	using OnHandledEvent = std::function<void(const sockaddr_in&, std::shared_ptr<SipMessage>)>;

	RequestsHandler(std::string serverIp, int serverPort,
		OnHandledEvent onHandledEvent);

	static std::shared_ptr<SipMessage> getMessageFromPool(std::string message, sockaddr_in src);

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

	// Ring/hunt groups. setRingGroup replaces a group's membership + mode; an empty
	// member list deletes the group. Thread-safe and NVS-persisted. The getter
	// returns {groupExt, "ringall"|"hunt", "m1,m2,..."} for the dashboard.
	void setRingGroup(const std::string& groupExt, const std::string& members, const std::string& mode);
	std::vector<std::tuple<std::string, std::string, std::string>> getRingGroups();

private:
	void initHandlers();

	// SIP request handlers (camelCase to match C++ convention)
	void onRegister(std::shared_ptr<SipMessage> data);
	void onOptions(std::shared_ptr<SipMessage> data);
	void onCancel(std::shared_ptr<SipMessage> data);
	void onReqTerminated(std::shared_ptr<SipMessage> data);
	void onInvite(std::shared_ptr<SipMessage> data);
	void onTrying(std::shared_ptr<SipMessage> data);
	void onRinging(std::shared_ptr<SipMessage> data);
	void onBusy(std::shared_ptr<SipMessage> data);
	void onUnavailable(std::shared_ptr<SipMessage> data);
	void onBye(std::shared_ptr<SipMessage> data);
	void onOk(std::shared_ptr<SipMessage> data);
	void onAck(std::shared_ptr<SipMessage> data);
	void onRefer(std::shared_ptr<SipMessage> data);   // blind transfer (RFC 3515)
	void onMessage(std::shared_ptr<SipMessage> data); // inbound MESSAGE (RFC 3428): ack 200 OK

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
	//   onOk() INVITE 200  : send ACK, then BYE, advance to AwaitingByeOk.
	//   onOk() BYE 200     : free the slot.
	//   tick() deadline    : if still AwaitingInviteOk, CANCEL and free; if
	//                        AwaitingByeOk, just free (best-effort BYE already sent).
	void sendRegisterBeep(const std::shared_ptr<SipClient>& phone);
	std::shared_ptr<SipMessage> buildBeepAck(const std::shared_ptr<SipMessage>& ok);
	std::shared_ptr<SipMessage> buildBeepBye(const std::shared_ptr<SipMessage>& ok);
	std::shared_ptr<SipMessage> buildBeepCancel(std::size_t slot);

	enum class BeepState { Free, AwaitingInviteOk, AwaitingByeOk };
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

	// Fan an INVITE out to a set of targets (the reusable core extracted from the
	// 999 all-page path). `targets` are pre-selected registered clients; `intercom`
	// adds the 999 auto-answer headers (true for 999, false for a ring group so it
	// rings normally). Builds the broadcast Session, the 180 Ringing to the caller,
	// and one forked INVITE per target. Caller holds _mutex.
	void startBroadcastFork(std::shared_ptr<SipMessage> invite,
		std::shared_ptr<SipClient> caller,
		std::vector<std::shared_ptr<SipClient>> targets,
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

	bool registerClient(std::shared_ptr<SipClient> client);
	void unregisterClient(std::string_view number);

	// Registration-lease handling (RFC 3261 §10.2.1)
	int parseRequestedExpires(const std::shared_ptr<SipMessage>& data) const;
	void sweepExpired();   // evict expired bindings; caller must hold _mutex
	void maybeSweep();     // throttled sweep; caller must hold _mutex

	std::optional<std::shared_ptr<SipClient>> findClient(std::string_view number);
	std::optional<std::shared_ptr<SipClient>> findClientByAddress(const sockaddr_in& addr);
	std::shared_ptr<SipMessage> buildOptionsPing(const std::shared_ptr<SipClient>& client);

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
	std::shared_ptr<SipClient> _dummyClient;

	// RequestsHandler.hpp: Issues #24 and #28 resolved.
	std::unordered_map<std::string, std::function<void(std::shared_ptr<SipMessage> request)>> _handlers;
	std::unordered_map<std::string, std::shared_ptr<Session>>   _sessions;

	std::mutex _mutex;
	OnHandledEvent _onHandled;
	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> _outbox;

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

	// NVS persistence for _forwards / _ringGroups. No-ops on host (the maps are the
	// store); on ESP they read/write the "pbxcfg" NVS namespace. Caller holds _mutex.
	void loadPbxConfig();                 // boot-time reload into the maps
	void persistForwards();               // write-through after a setForward mutation
	void persistRingGroups();             // write-through after a setRingGroup mutation
	bool _pbxConfigLoaded = false;

	// Persistent CDR (Class A sweep). The CDR ring is flushed to the "cdrlog" NVS
	// namespace on teardown (write-through) and reloaded on boot, so records survive
	// reboot. No-ops on host. Caller holds _mutex.
	void loadCdrRing();                   // boot-time reload of the ring
	void persistCdrRing();                // flush the whole ring (bounded, fixed size)

	// Pre-allocated static memory pools (Issue #53)
	std::vector<std::shared_ptr<SipClient>> _clientPool;
	std::vector<std::shared_ptr<Session>> _sessionPool;
	static std::vector<std::shared_ptr<SipMessage>> _messagePool;

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
};

#endif
