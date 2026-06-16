// RequestsHandler.cpp: Issues #24 and #28 resolved.
#include "RequestsHandler.hpp"
#include <atomic>
#include <sstream>
#include <thread>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include "SipMessageTypes.h"
#include "SipSdpMessage.hpp"
#include "IDGen.hpp"
#include "IPHelper.hpp"
#include "PoolConfig.hpp"
#include "CallDetailRecord.hpp"
#include "PbxConfig.hpp"
#include "AdminAuth.hpp"
#include "SipDigest.hpp"
#include "SipSecretStore.hpp"
#include "ArpLookup.hpp"

#if defined(ESP_PLATFORM) || defined(ESP32)
	// PBX config (call-forward / ring groups) and the persistent CDR ring live in
	// NVS on the device. nvs_flash/nvs are core ESP-IDF components present on every
	// transport, so they gate on ESP_PLATFORM (not POCKETDIAL_HAS_WIFI). On host the
	// in-memory maps/ring ARE the store and these calls compile out (see the
	// load*/persist* helpers at the bottom of this file).
	#include "nvs_flash.h"
	#include "nvs.h"
	#include "esp_sntp.h"
	#include "esp_system.h"
	#include "esp_idf_version.h"
	#include <cstring>
#endif

std::vector<std::shared_ptr<SipMessage>> RequestsHandler::_messagePool;
std::mutex RequestsHandler::_messagePoolMutex;

namespace
{
	// Default lease granted when a REGISTER does not request one (seconds).
	constexpr int DEFAULT_EXPIRES = 3600;
	// Upper bound we are willing to grant, regardless of what the client asks.
	constexpr int MAX_EXPIRES = 3600;
	// Minimum non-zero lease, to avoid pathologically short registrations.
	constexpr int MIN_EXPIRES = 30;
	// How often the opportunistic sweep is allowed to run.
	constexpr auto SWEEP_INTERVAL = std::chrono::seconds(1);

	// How long an unanswered leg rings before the no-answer action fires (CFNA
	// forward, or advancing to the next hunt-group member). Polled from tick().
	constexpr auto NO_ANSWER_TIMEOUT = std::chrono::seconds(20);

	// NVS namespaces / keys for the persisted PBX config and CDR ring. Kept short
	// (NVS keys are capped at 15 chars). Forward/group entries are stored one blob
	// per extension so a single mutation rewrites only its own key (low flash wear).
	constexpr auto NVS_PBX_NS  = "pbxcfg";
	constexpr auto NVS_CDR_NS  = "cdrlog";
}

RequestsHandler::RequestsHandler(std::string serverIp, int serverPort,
	OnHandledEvent onHandledEvent) :
	_onHandled(onHandledEvent),
	_serverIp(std::move(serverIp)),
	_serverPort(serverPort)
{
	initHandlers();
	// Pre-allocate pools (Issue #53). Capacities are compile-time tunable via
	// PoolConfig.hpp (-DPOCKETDIAL_MAX_* overrides); defaults preserve 32/8/32.
	_clientPool.reserve(POCKETDIAL_MAX_CLIENTS);
	for (int i = 0; i < POCKETDIAL_MAX_CLIENTS; ++i)
	{
		_clientPool.push_back(std::make_shared<SipClient>());
	}
	_sessionPool.reserve(POCKETDIAL_MAX_SESSIONS);
	for (int i = 0; i < POCKETDIAL_MAX_SESSIONS; ++i)
	{
		_sessionPool.push_back(std::make_shared<Session>());
	}
	// Issue #70: pre-allocate the virtual-peer pool (777/440/park/PSTN legs) so the
	// packet handler never make_shares a SipClient in the hot path.
	_virtualPeerPool.reserve(POCKETDIAL_VIRTUAL_PEERS);
	for (int i = 0; i < POCKETDIAL_VIRTUAL_PEERS; ++i)
	{
		_virtualPeerPool.push_back(std::make_shared<SipClient>());
	}
	if (_messagePool.empty())
	{
		_messagePool.reserve(POCKETDIAL_MSG_POOL);
		for (int i = 0; i < POCKETDIAL_MSG_POOL; ++i)
		{
			_messagePool.push_back(std::make_shared<SipSdpMessage>("", sockaddr_in{}));
		}
	}

	// Reload persisted PBX config (call-forward / ring groups) and the CDR ring from
	// NVS so they survive reboot. No-ops on host. Construction is single-threaded
	// (no handler is dispatching yet), so these run without holding _mutex.
	loadPbxConfig();
	loadCdrRing();
	// Task 2B: load the admin extension from NVS (defaults to "101" if absent).
	loadAdminExt();
	// STAGE 2: load the registrar mode (defaults to the POCKETDIAL_OPEN_REGISTRAR
	// seed) and the adopted-device registry from NVS.
	loadRegistrarMode();
	loadDevices();
	loadThreeCxConfig();
	// #107: anchor TLS re-warm cadence (defaults to 60 min if the NVS key is absent).
	loadRewarmInterval();
	// Prewarm the per-extension HA1 cache off the REGISTER hot path so the first
	// Secure REGISTER does not pay a blocking NVS read while holding _mutex.
	SipSecretStore::warmCache();
	// Seed the dashboard snapshot with the loaded devices so the TUI sees adopted
	// devices immediately on boot (online flags start false until each re-REGISTERs).
	refreshDeviceSnapshot();
}

RequestsHandler::~RequestsHandler()
{
	// Issue #42/#55: flush any pending debounced NVS writes one last time so a final
	// teardown CDR / config edit that landed after the last tick still survives. No-op
	// on host. tick() is no longer running by destruction time, so this takes the lock
	// itself; no concurrent writer remains.
	flushDirtyNvs();
#if !defined(ESP_PLATFORM) && !defined(ESP32)
	if (_anchorStartThread.joinable())
	{
		_anchorStartThread.join();
	}
	// Issue #67: drain+join all remaining anchor workers (tick() reaps finished ones
	// during normal operation; this catches any still in flight at teardown).
	reapAnchorWorkers(/*drainAll=*/true);
#endif
	// Stop the anchor BEFORE member destruction begins: the loopback client's
	// simulation threads call back into this handler (locking _mutex, touching
	// _sessions), and those members are destroyed before _loopbackClient itself —
	// relying on ~LoopbackAnchorClient to join them is a use-after-free.
	if (_anchorClient)
	{
		_anchorClient->stop();
	}
}

std::shared_ptr<SipMessage> RequestsHandler::getMessageFromPool(std::string message, sockaddr_in src)
{
	// Issue #41 (C-1): the check-then-act below (use_count()==1 → reset) runs on the
	// UDP RX thread (via SipMessageFactory::createMessage, BEFORE handle() takes
	// _mutex) AND from tick()/handlers (under _mutex). _mutex does not serialise the
	// two callers, so two threads could otherwise both observe the same idle slot,
	// reset it, and hand out the same buffer (UB: corrupt message, reset mid-parse).
	// A dedicated leaf mutex makes the scan+reset atomic. It is a LEAF: held only for
	// the brief scan/reset, never around socket/NVS I/O, and never nested inside
	// itself, so it cannot deadlock against _mutex.
	std::lock_guard<std::mutex> poolLock(_messagePoolMutex);
	for (auto& msg : _messagePool)
	{
		if (msg.use_count() == 1)
		{
			msg->reset(std::move(message), src);
			return msg;
		}
	}
	std::cerr << "[WARNING] SIP Message pool exhausted! Fallback to heap allocation.\n";
	return std::make_shared<SipSdpMessage>(std::move(message), src);
}

void RequestsHandler::initHandlers()
{
	_handlers.emplace(SipMessageTypes::REGISTER,          std::bind(&RequestsHandler::onRegister,       this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::OPTIONS,           std::bind(&RequestsHandler::onOptions,        this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::CANCEL,            std::bind(&RequestsHandler::onCancel,         this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::INVITE,            std::bind(&RequestsHandler::onInvite,         this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::TRYING,            std::bind(&RequestsHandler::onTrying,         this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::RINGING,           std::bind(&RequestsHandler::onRinging,        this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::BUSY,              std::bind(&RequestsHandler::onBusy,           this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::UNAVAILABLE,       std::bind(&RequestsHandler::onUnavailable,    this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::OK,                std::bind(&RequestsHandler::onOk,             this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::ACK,               std::bind(&RequestsHandler::onAck,            this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::BYE,               std::bind(&RequestsHandler::onBye,            this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::REQUEST_TERMINATED,std::bind(&RequestsHandler::onReqTerminated,  this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::REFER,             std::bind(&RequestsHandler::onRefer,          this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::MESSAGE,           std::bind(&RequestsHandler::onMessage,        this, std::placeholders::_1));
	_handlers.emplace(SipMessageTypes::SUBSCRIBE,         std::bind(&RequestsHandler::onSubscribe,      this, std::placeholders::_1));
}

void RequestsHandler::handle(std::shared_ptr<SipMessage> request)
{
	// Input validation: Drop null or structurally malformed packets instantly (SEC-02)
	if (!request || !request->isValidMessage())
	{
		_packetsDropped.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> localOutbox;
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);

		// Per-source IP Rate Limiting (Issue #38 / SEC-02)
		if (!ipAllowed(request->getSource()) || !allowPacket(request->getSource()))
		{
			_packetsDropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		_packetsProcessed.fetch_add(1, std::memory_order_relaxed);
		_outbox.clear();

		auto client = findClientByAddress(request->getSource());
		if (client.has_value())
		{
			client.value()->markActive();
		}

		maybeSweep();

		// Route responses by parsed numeric status code so dispatch is immune to
		// reason-phrase variation (e.g. "486 Busy" vs "486 Busy Here"). Requests and
		// anything without a status line fall back to the method/start-line token.
		std::string handlerKey;
		auto status = request->getStatusInfo();
		if (status.has_value())
		{
			switch (status->code)
			{
				case 100: handlerKey = SipMessageTypes::TRYING;             break;
				case 180: handlerKey = SipMessageTypes::RINGING;            break;
				case 200: handlerKey = SipMessageTypes::OK;                 break;
				case 480: handlerKey = SipMessageTypes::UNAVAILABLE;        break;
				case 486: handlerKey = SipMessageTypes::BUSY;               break;
				case 487: handlerKey = SipMessageTypes::REQUEST_TERMINATED; break;
				default:  handlerKey = std::string(request->getType());     break;
			}
		}
		else
		{
			handlerKey = std::string(request->getType());
		}

		// DIAGNOSTIC: log inbound dialog-shaping requests so we can see whether the
		// handset ACKs our 200 OK (vs CANCEL/re-INVITE). REGISTER/OPTIONS excluded.
		if (handlerKey == "ACK" || handlerKey == "BYE" || handlerKey == "CANCEL" || handlerKey == "INVITE")
		{
			dumpWire("[SIP IN] ", handlerKey, request->getCallID(), request->getToNumber());
		}

		// Surface inbound client-error (4xx) responses once. Deferred via _logQueue
		// so the write happens outside the lock (Issue #24); replaces the per-parse
		// printf the parser used to do. softFail -> warning (stdout), else error (stderr).
		if (status.has_value() && status->klass == PocketDial::SipStatusClass::ClientError)
		{
			// A 487/481 that belongs to a register-beep dialog is expected teardown-race
			// noise — the phone finalising our cancelled beep INVITE (#90), not a fault.
			// Log it as a benign beep event so it never reads as a teardown failure.
			if ((status->code == 487 || status->code == 481) &&
				findBeepByCallID(request->getCallID()) != nullptr)
			{
				queueLog("[SIP] beep teardown: " + std::to_string(status->code)
					+ " (expected cancel race)");
			}
			else
			{
				queueLog("[SIP] " + std::string(status->softFail ? "WARN " : "ERROR ")
					+ std::string(request->getHeader()), !status->softFail);
			}
		}

		// Task 2C: SIP INFO with DTMF relay body — handle before the handler table
		// so it is never mistakenly forwarded by a catch-all entry.
		if (handlerKey == "INFO")
		{
			// Scan headers for Content-Type: application/dtmf-relay.
			const std::string& rawMsg = request->toString();
			bool isDtmfRelay = false;
			{
				size_t pos = 0;
				while (pos < rawMsg.size())
				{
					size_t nl = rawMsg.find('\n', pos);
					size_t next = (nl == std::string::npos) ? rawMsg.size() : nl + 1;
					// Header/body boundary: blank line.
					if (pos < rawMsg.size() && (rawMsg[pos] == '\r' || rawMsg[pos] == '\n')) break;
					// Header name = text before the first ':' (RFC 3261: no WS before colon).
					size_t colon = rawMsg.find(':', pos); if (colon != std::string::npos && colon < ((nl == std::string::npos) ? rawMsg.size() : nl))
					{
						std::string nameLC = rawMsg.substr(pos, colon - pos);
						std::transform(nameLC.begin(), nameLC.end(), nameLC.begin(),
							[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
						if (nameLC == "content-type" || nameLC == "c")
						{
							size_t valEnd = (nl == std::string::npos) ? rawMsg.size() : nl;
							std::string val = rawMsg.substr(colon + 1, valEnd - (colon + 1));
							if (val.find("application/dtmf-relay") != std::string::npos)
							{
								isDtmfRelay = true;
							}
							break;
						}
					}
					pos = next;
				}
			}
			// Always 200 OK a SIP INFO (RFC 6086 §4.2.1).
			{
				auto infoOk = getMessageFromPool(request->toString(), request->getSource());
				infoOk->setHeader(SipMessageTypes::OK);
				std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
				infoOk->setVia(std::string(request->getVia()) + ";received=" + activeIp);
				_outbox.emplace_back(request->getSource(), std::move(infoOk));
			}
			if (isDtmfRelay)
			{
				onDtmfInfo(request);
			}
		}
		else
		{
			auto it = _handlers.find(handlerKey);
			if (it != _handlers.end())
			{
				it->second(std::move(request));
			}
		}

		// BLF change detection: one pass after every handled packet covers
		// registration appear/disappear, session create/transition/teardown.
		// NOTIFYs land in _outbox and ride out with this pass (after unlock).
		refreshSubscriptions();

		localOutbox = std::move(_outbox);
		_outbox.clear();
		// Append any out-of-band sends (WS Answered/Dropped callbacks) so they ride
		// out on this pass instead of being wiped by the next start-of-body clear.
		for (auto& e : _asyncOutbox) localOutbox.push_back(std::move(e));
		_asyncOutbox.clear();

		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	// Print deferred logs safely outside of the lock
	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}

	// Issue #24 resolved: UDP socket syscall sendto is now executed outside the locked section to prevent lock contention.
	for (auto& event : localOutbox)
	{
		_onHandled(event.first, std::move(event.second));
	}
}

std::optional<std::shared_ptr<Session>> RequestsHandler::getSession(std::string_view callID)
{
	auto sessionIt = _sessions.find(std::string(callID));
	if (sessionIt != _sessions.end())
	{
		return sessionIt->second;
	}
	return {};
}

void RequestsHandler::onRegister(std::shared_ptr<SipMessage> data)
{
	auto fromNumber = data->getFromNumber();
	int requestedExpires = parseRequestedExpires(data);
	int grantedExpires = 0;

	if (!isValidAor(fromNumber))
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader("SIP/2.0 400 Bad Request");
		response->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}

	// ── Registrar-mode admission (STAGE 2) ───────────────────────────────────────
	// Runtime policy replaces the old compile-time POCKETDIAL_OPEN_REGISTRAR gate.
	//   Open   : accept every REGISTER (legacy standalone behaviour).
	//   Secure : digest-challenge + verify against the stored HA1 for this ext.
	//   Learn  : TOFU + MAC-lock — adopt unknown devices, enforce secured ones.
	// On Challenge the helper has already enqueued the 401 + WWW-Authenticate; on
	// Reject we emit the 403 here from rejectReason. Either way a non-Accept stops.
	const std::string extStr(fromNumber);
	const RegistrarMode mode = _registrarMode.load(std::memory_order_relaxed);
	if (mode != RegistrarMode::Open)
	{
		std::string rejectReason;
		AuthDecision decision = (mode == RegistrarMode::Secure)
			? admitSecure(data, extStr, rejectReason)
			: admitLearn(data, extStr, rejectReason);

		if (decision == AuthDecision::Challenge)
		{
			// admitSecure already enqueued the 401 + WWW-Authenticate.
			return;
		}
		if (decision == AuthDecision::Reject)
		{
			sendForbidden(data, rejectReason.empty() ? "Forbidden" : rejectReason);
			return;
		}
		// decision == Accept → fall through to the normal binding path below.
	}

	// Resolve the device MAC once (Learn/registry bookkeeping). nullopt on a
	// first-packet ARP miss or on host — the online flag just stays unchanged then.
	std::optional<std::string> deviceMac;
	{
		auto m = ArpLookup::pdLookupMac(data->getSource());
		if (m.has_value()) deviceMac = ArpLookup::toHex12(*m);
	}

	if (requestedExpires <= 0)
	{
		// expires=0 (or an explicit zero) is a de-registration request.
		unregisterClient(fromNumber);
		if (deviceMac.has_value()) markDeviceOnline(*deviceMac, false);
	}
	else
	{
		grantedExpires = (std::max)(MIN_EXPIRES, (std::min)(requestedExpires, MAX_EXPIRES));
		// Distinguish a brand-new binding from a lease refresh BEFORE allocating: a
		// client already present in the pool under this number is a re-REGISTER, which
		// must NOT trigger a welcome MESSAGE (phones re-register every lease period).
		bool isNewBinding = !findClient(fromNumber).has_value();
		// Always update address so re-REGISTER after a NAT rebind works correctly
		auto newClient = allocateClient(std::string(data->getFromNumber()), data->getSource(), grantedExpires);
		if (newClient)
		{
			registerClient(newClient);
			// Register beep: on a brand-new binding ONLY (never a lease refresh —
			// phones re-REGISTER every lease period), send the registering phone a
			// brief intercom auto-answer INVITE so it plays its own tone, then tear
			// the call back down. Signaling-only: the server sources NO RTP. Bounded
			// and best-effort — if the beep table is full the beep is simply skipped.
			if (isNewBinding)
			{
				sendRegisterBeep(newClient);
			}
			if (deviceMac.has_value()) markDeviceOnline(*deviceMac, true);
		}
		else
		{
			// Server full: reply with 503 Service Unavailable
			auto response = getMessageFromPool(data->toString(), data->getSource());
			response->setHeader("SIP/2.0 503 Service Unavailable");
			response->clearBody();
			std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
			response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
			_outbox.emplace_back(data->getSource(), std::move(response));
			return;
		}
	}

	auto response = getMessageFromPool(data->toString(), data->getSource());
	response->setHeader(SipMessageTypes::OK);
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	response->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
	// Echo the granted lease back in the Contact so the client knows when to refresh.
	response->setContact(buildContact(fromNumber) + ";expires=" + std::to_string(grantedExpires));
	endHandle(fromNumber, response);
}

void RequestsHandler::onOptions(std::shared_ptr<SipMessage> data)
{
	auto response = getMessageFromPool(data->toString(), data->getSource());
	response->setHeader(SipMessageTypes::OK);
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	response->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
	response->setContact(buildContact(data->getFromNumber()));
	_outbox.emplace_back(data->getSource(), std::move(response));
}

void RequestsHandler::onCancel(std::shared_ptr<SipMessage> data)
{
	std::string destNumber(data->getToNumber());

	// Issue #46: same off-path teardown guard as onBye(). A CANCEL whose Call-ID
	// names an established two-leg dialog must come from a leg IP. Early-dialog
	// CANCELs (dest not yet bound — ring/hunt fork still ringing) fail open via the
	// helper, which is conservative: only an established call is hard-gated here.
	if (auto cancelSess = getSession(data->getCallID());
		cancelSess.has_value() &&
		!isDialogSourceAuthorized(cancelSess.value(), data->getSource()))
	{
		queueLog("CANCEL for Call-ID " + std::string(data->getCallID()) +
			" rejected: source not a dialog leg (spoofed teardown)", true);
		sendForbidden(data, "Forbidden");
		return;
	}

	if (destNumber == "777")
	{
		endCall(data->getCallID(), data->getFromNumber(), "777");
		return;
	}

	if (destNumber == "440")
	{
		// CANCEL of the media call: stop the stream (if it owns this Call-ID) and end.
		_rtpSender.stop(std::string(data->getCallID()));
		endCall(data->getCallID(), data->getFromNumber(), "440");
		return;
	}

	if (parkOrbitIndex(destNumber) >= 0)
	{
		// CANCEL of a parking/retrieving INVITE: tear the leg down. endCall() also
		// frees the orbit slot if this Call-ID owns one (freeParkSlot hook).
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		refreshParkSnapshot();
		return;
	}

	if (destNumber == "999" || isPageZoneDialog(destNumber))
	{
		auto session = getSession(data->getCallID());
		if (session.has_value())
		{
			std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
			std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);
			std::string originalCSeq(data->getCSeq());
			size_t invitePos = originalCSeq.find("INVITE");
			if (invitePos != std::string::npos)
			{
				originalCSeq.replace(invitePos, 6, "CANCEL");
			}

			for (const auto& target : session.value()->getPendingTargets())
			{
				auto cancelMsg = getMessageFromPool(data->toString(), data->getSource());
				char ipBuf[INET_ADDRSTRLEN]{};
				inet_ntop(AF_INET, &target->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
				std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(target->getAddress().sin_port));

				cancelMsg->setHeader("CANCEL sip:" + target->getNumber() + "@" + targetIpPort + " SIP/2.0");

				std::string newTo = "To: <sip:" + target->getNumber() + "@" + serverIpPort + ">";
				cancelMsg->setTo(newTo);
				cancelMsg->setCSeq(originalCSeq);
				_outbox.emplace_back(target->getAddress(), std::move(cancelMsg));
			}
		}
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	{
		auto session = getSession(data->getCallID());
		if (session.has_value() && session.value()->isAnchor())
		{
			_mediaBridge.stopBridge();
			asyncDropCall(session.value()->getAnchorParticipantId());   // drop the PSTN leg by id
			auto response = getMessageFromPool(data->toString(), data->getSource());
			response->setHeader(SipMessageTypes::OK);
			std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
			response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
			_outbox.emplace_back(data->getSource(), std::move(response));
			endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), "handset CANCEL");
			return;
		}
	}

	setCallState(data->getCallID(), Session::State::Cancel);
	endHandle(data->getToNumber(), data);
}

void RequestsHandler::onReqTerminated(std::shared_ptr<SipMessage> data)
{
	// Register-beep 487: the phone's final response to a beep INVITE we CANCELled (or
	// that the phone terminated on its own). RFC 3261 §17.1.1.3 — the UAC MUST ACK a
	// non-2xx final response within the INVITE transaction (same branch). The #90 bug
	// freed the dialog the instant the CANCEL was queued, so this 487 found no slot, was
	// never ACKed, and the phone retransmitted it. ACK it, THEN free — the BEAST's
	// "complete the teardown before releasing the leg" discipline (cf. lnp-audit).
	if (BeepDialog* bd = findBeepByCallID(data->getCallID()))
	{
		if (bd->state == BeepState::AwaitingInviteOk ||
			bd->state == BeepState::AwaitingCancelDone)
		{
			auto ack = buildBeepAck(data);   // same INVITE branch, To-tag from the 487
			if (ack) _outbox.emplace_back(bd->addr, std::move(ack));
		}
		*bd = BeepDialog{};   // INVITE transaction complete — now release the slot
		return;
	}

	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isBroadcast())
	{
		return;
	}
	// Inbound ring-all: a 487 is a forked loser's reply to the CANCEL we sent when another
	// leg won (or on teardown). Complete the cancelled INVITE transaction with an ACK; there
	// is no caller to relay it to.
	if (session.has_value() && session.value()->isAnchorInbound())
	{
		ackInboundFinal(session.value(), data);
		return;
	}
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onInvite(std::shared_ptr<SipMessage> data)
{
	// Task 2A: Retransmission guard — silently drop if a session for this Call-ID
	// is already active (Invited or Connected). RFC 3261 §17.2.3: a UAS that receives
	// a retransmission of a request for which a non-2xx final response has been sent
	// should retransmit that response; for 2xx, the ACK re-drive handles it. The
	// simplest safe policy here is a silent drop so we never create a second session
	// slot for the same dialog, which could exhaust the pool and trigger spurious 503.
	if (auto existing = getSession(data->getCallID()); existing.has_value())
	{
		const auto st = existing.value()->getState();
		// Mid-dialog re-INVITE (RFC 3261 §12.2): an INVITE whose To header
		// already carries a tag belongs to the established dialog — this is the
		// hold/resume path, NOT a retransmission. Route it to onReinvite().
		if ((st == Session::State::Connected || st == Session::State::Held) &&
			std::string_view(data->getTo()).find("tag=") != std::string_view::npos)
		{
			onReinvite(data);
			return;
		}
		if (st == Session::State::Invited || st == Session::State::Connected ||
			st == Session::State::Held)
		{
			return; // silent drop per RFC 3261 §17.2.3
		}
	}

	if (!isValidAor(data->getFromNumber()) || !isValidAor(data->getToNumber()))
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader("SIP/2.0 400 Bad Request");
		response->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}

	// Check if the caller is registered
	auto caller = findClient(data->getFromNumber());
	if (!caller.has_value())
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader("SIP/2.0 403 Forbidden");
		response->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}

	std::string destNumber(data->getToNumber());
	if (destNumber == "777")
	{
		// SDP loopback echo test
		auto ringing = getMessageFromPool(data->toString(), data->getSource());
		ringing->setHeader("SIP/2.0 180 Ringing");
		ringing->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		ringing->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		std::string toTag = IDGen::GenerateID(9);
		ringing->setTo(std::string(data->getTo()) + ";tag=" + toTag);
		ringing->setContact(buildContact("777"));
		_outbox.emplace_back(data->getSource(), std::move(ringing));

		auto okResponse = getMessageFromPool(data->toString(), data->getSource());
		okResponse->setHeader(SipMessageTypes::OK);
		okResponse->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		okResponse->setTo(std::string(data->getTo()) + ";tag=" + toTag);
		okResponse->setContact(buildContact("777"));
		okResponse->enforceG711();
		_outbox.emplace_back(data->getSource(), std::move(okResponse));

		auto virtualClient = allocateVirtualPeer("777", data->getSource());
		auto newSession = allocateSession(std::string(data->getCallID()), caller.value());
		if (newSession)
		{
			newSession->setDest(virtualClient);
			_sessions.emplace(data->getCallID(), newSession);
			newSession->setState(Session::State::Connected);
		}
		return;
	}

	if (destNumber == "999")
	{
		// All Page / Broadcast: fork to every other registered client, with the
		// intercom auto-answer headers. Targets are picked here; the fork machinery
		// is the shared startBroadcastFork() helper (also used by ring groups).
		std::vector<std::shared_ptr<SipClient>> targets;
		for (const auto& client : _clientPool)
		{
			if (!client->getNumber().empty() && client->getNumber() != caller.value()->getNumber())
			{
				targets.push_back(client);
			}
		}

		if (targets.empty())
		{
			std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
			responseObj->setHeader(SipMessageTypes::NOT_FOUND);
			responseObj->clearBody();
			responseObj->setContact(buildContact(caller.value()->getNumber()));
			_outbox.emplace_back(data->getSource(), std::move(responseObj));
			return;
		}

		startBroadcastFork(data, caller.value(), std::move(targets), /*intercom=*/true);
		return;
	}

	// Paging zones (980–989): a configured zone is a scoped 999 — fork an intercom
	// (auto-answer) INVITE to every registered zone member through exactly the same
	// startBroadcastFork() machinery (first answer wins, onOk cancels the rest).
	// An unconfigured 98x falls through to normal routing (and 404s, since 98x is
	// rejected as a real extension/group/forward target).
	if (pbx::isPageZoneExt(destNumber))
	{
		if (const pbx::PageZone* zone = findPageZone(destNumber))
		{
			std::vector<std::shared_ptr<SipClient>> targets;
			for (const auto& m : zone->members)
			{
				if (m == caller.value()->getNumber()) continue;
				auto mc = findClient(m);
				if (mc.has_value())
				{
					targets.push_back(mc.value());
				}
			}

			if (targets.empty())
			{
				std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
				responseObj->setHeader(SipMessageTypes::UNAVAILABLE);
				responseObj->clearBody();
				responseObj->setContact(buildContact(caller.value()->getNumber()));
				_outbox.emplace_back(data->getSource(), std::move(responseObj));
				return;
			}

			startBroadcastFork(data, caller.value(), std::move(targets), /*intercom=*/true);
			return;
		}
	}

	if (destNumber == "440")
	{
		// Media beachhead: the server answers and sources a one-way RTP tone stream.
		onMediaInvite(data, caller.value());
		return;
	}

	// Call parking (park-orbit, 700..70N): an INVITE to a FREE orbit parks the
	// caller's leg there; an INVITE to an OCCUPIED orbit retrieves the parked call
	// (the server re-INVITEs the parked party toward the retriever's media — no
	// media is ever held here). Resolved before ring groups/DND like 777/999.
	{
		int orbitIdx = parkOrbitIndex(destNumber);
		if (orbitIdx >= 0)
		{
			onParkInvite(data, caller.value(), orbitIdx);
			return;
		}
	}

	// Ring / hunt groups (Class A sweep): a configured group extension (e.g. 6xx)
	// maps to an ordered member list. Ring-all reuses the broadcast fork (without the
	// intercom auto-answer headers, so members ring normally); hunt rings members one
	// at a time, driven from tick(). Resolved before DND because a group ext is not a
	// real endpoint and so never carries its own DND/forward config.
	if (const pbx::RingGroup* group = findRingGroup(destNumber))
	{
		// Collect the registered members (skip the caller and any offline member).
		std::vector<std::shared_ptr<SipClient>> members;
		std::vector<std::string> huntOrder;
		for (const auto& m : group->members)
		{
			if (m == caller.value()->getNumber()) continue;
			auto mc = findClient(m);
			if (mc.has_value())
			{
				members.push_back(mc.value());
				huntOrder.push_back(m);
			}
		}

		if (members.empty())
		{
			std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
			responseObj->setHeader(SipMessageTypes::UNAVAILABLE);
			responseObj->clearBody();
			responseObj->setContact(buildContact(caller.value()->getNumber()));
			endHandle(data->getFromNumber(), responseObj);
			return;
		}

		if (group->mode == pbx::GroupMode::RingAll)
		{
			startBroadcastFork(data, caller.value(), std::move(members), /*intercom=*/false);
			return;
		}

		// Hunt (sequential): build a broadcast-style session but ring one at a time.
		auto newSession = allocateSession(std::string(data->getCallID()), caller.value());
		if (!newSession)
		{
			std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
			responseObj->setHeader("SIP/2.0 503 Service Unavailable");
			responseObj->clearBody();
			responseObj->setContact(buildContact(caller.value()->getNumber()));
			_outbox.emplace_back(data->getSource(), std::move(responseObj));
			return;
		}
		newSession->setBroadcast(true);
		newSession->setHunt(true);
		newSession->setGroupExt(destNumber);
		newSession->setInviteMessage(data);
		newSession->setHuntMembers(std::move(huntOrder));
		newSession->setHuntIndex(0);
		_sessions.emplace(data->getCallID(), newSession);

		// 180 Ringing back to the caller while we walk the list.
		auto ringing = getMessageFromPool(data->toString(), data->getSource());
		ringing->setHeader("SIP/2.0 180 Ringing");
		ringing->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		ringing->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		ringing->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
		ringing->setContact(buildContact(destNumber));
		_outbox.emplace_back(data->getSource(), std::move(ringing));

		huntRingNext(newSession);   // ring the first member, arm its timeout
		return;
	}

	// Call Forward Unconditional (CFU): if the destination has an "always" forward
	// target, redirect the call to that target before ringing the original callee.
	{
		std::string cfu = getForwardTarget(destNumber, "always");
		if (!cfu.empty() && cfu != destNumber)
		{
			queueLog("CFU: forwarding " + destNumber + " -> " + cfu);
			if (redirectInvite(data, caller.value(), cfu))
			{
				return;
			}
			// Forward target offline: fall through and try the original callee.
		}
	}

	// Do Not Disturb (Phase 2): if the target extension has DND enabled, decline
	// with 480 Temporarily Unavailable instead of ringing it. This branch is reached
	// only for ordinary extensions — the virtual 777 (echo) and 999 (broadcast)
	// extensions are handled above and so are never affected by DND.
	if (isDndEnabled(destNumber))
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader("SIP/2.0 480 Temporarily Unavailable");
		response->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		response->setContact(buildContact(caller.value()->getNumber()));
		endHandle(data->getFromNumber(), response);
		return;
	}

	// Dial plan: a leading '9' is the explicit trunk-access prefix. "101" rings
	// LAN extension 101; "9101" strips the 9 and places 101 via the WAN anchor
	// (3CX call-control API) — even if a local "9101" could exist. Checked before
	// the registrar lookup so the prefix always means "go out the trunk".
	if (destNumber.size() >= 2 && destNumber[0] == '9' &&
	    _anchorClient && _anchorClient->isConnected())
	{
		routeAnchorCall(data, caller.value(), destNumber.substr(1));
		return;
	}

	// Check if the called is registered
	auto called = findClient(data->getToNumber());
	if (!called.has_value())
	{
		// Outbound WAN call via 3CX Media Anchor (legacy fallback: an unregistered
		// destination without the 9-prefix still tries the trunk when one is up).
		if (_anchorClient && _anchorClient->isConnected())
		{
			routeAnchorCall(data, caller.value(), destNumber);
			return;
		}

		// Send "SIP/2.0 404 Not Found"
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
		responseObj->setHeader(SipMessageTypes::NOT_FOUND);
		responseObj->clearBody();
		responseObj->setContact(buildContact(caller.value()->getNumber()));
		endHandle(data->getFromNumber(), responseObj);
		return;
	}

	SipSdpMessage* message = nullptr;
	if (data && data->hasSdp())
	{
		message = static_cast<SipSdpMessage*>(data.get());
	}
	if (!message) 
	{
		queueLog("Couldn't get SDP from " + std::string(data->getFromNumber()) + "'s INVITE request.", true);
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
		responseObj->setHeader(SipMessageTypes::BAD_REQUEST);
		responseObj->clearBody();
		responseObj->setContact(buildContact(caller.value()->getNumber()));
		endHandle(data->getFromNumber(), responseObj);
		return;
	}

	auto newSession = allocateSession(std::string(data->getCallID()), caller.value());
	if (!newSession)
	{
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
		responseObj->setHeader("SIP/2.0 503 Service Unavailable");
		responseObj->clearBody();
		responseObj->setContact(buildContact(caller.value()->getNumber()));
		endHandle(data->getFromNumber(), responseObj);
		return;
	}
	_sessions.emplace(data->getCallID(), newSession);

	// Retain the original INVITE on the session if the callee has ANY conditional
	// forward (busy or no-answer), so onBusy()/tick() can re-drive it to the forward
	// target. CFB (busy) is handled in onBusy(); CFNA arms a one-shot ring timer that
	// tick() fires after NO_ANSWER_TIMEOUT (CANCEL original leg, INVITE the target).
	std::string cfb  = getForwardTarget(destNumber, "busy");
	std::string cfna = getForwardTarget(destNumber, "noanswer");
	if (!cfb.empty() || !cfna.empty())
	{
		newSession->setInviteMessage(data);
	}
	if (!cfna.empty() && cfna != destNumber)
	{
		newSession->setNoAnswerTarget(cfna);
		newSession->armRingTimer(std::chrono::steady_clock::now() + NO_ANSWER_TIMEOUT);
	}

	auto response = getMessageFromPool(data->toString(), data->getSource());
	response->setContact(buildContact(caller.value()->getNumber()));
	endHandle(data->getToNumber(), response);
}

// Leg identity for re-INVITE routing: compare the packet source against a
// session leg's registered address (IP + port).
static bool sameAddress(const sockaddr_in& a, const sockaddr_in& b)
{
	return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

void RequestsHandler::onReinvite(std::shared_ptr<SipMessage> data)
{
	auto sessionOpt = getSession(data->getCallID());
	if (!sessionOpt.has_value())
	{
		return;
	}
	auto session = sessionOpt.value();
	auto src = session->getSrc();
	auto dest = session->getDest();

	// Virtual / anchor legs (777 echo, 440 tone, WAN anchor) have no real peer
	// phone to relay an offer to — decline the re-INVITE so the holding phone
	// keeps the call up on the original SDP.
	const std::string destNum = dest ? dest->getNumber() : "";
	if (session->isAnchor() || session->isAnchorInbound() ||
		destNum == "777" || destNum == "440" || !src || !dest)
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader("SIP/2.0 488 Not Acceptable Here");
		response->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}

	// Identify the sending leg by source address; the relay target is the peer.
	std::shared_ptr<SipClient> peer;
	if (sameAddress(data->getSource(), src->getAddress()))
	{
		peer = dest;
	}
	else if (sameAddress(data->getSource(), dest->getAddress()))
	{
		peer = src;
	}
	if (!peer)
	{
		return; // not from either leg of this dialog: ignore
	}

	// Relay UNTOUCHED — no clearBody()/enforceG711() — so the hold SDP
	// (a=sendonly/inactive) and its Content-Length reach the peer intact.
	_outbox.emplace_back(peer->getAddress(), data);

	// Track hold state from the offered SDP direction. RFC 3264: an absent
	// direction attribute implies sendrecv (an active call).
	const auto dir = data->getSdpDirection();
	if (dir == SipMessage::SdpDirection::SendOnly ||
		dir == SipMessage::SdpDirection::RecvOnly ||
		dir == SipMessage::SdpDirection::Inactive)
	{
		session->setState(Session::State::Held);
		queueLog("Hold: " + std::string(data->getFromNumber()) + " held call " + std::string(data->getCallID()));
	}
	else
	{
		session->setState(Session::State::Connected);
		queueLog("Hold: call " + std::string(data->getCallID()) + " resumed");
	}

	// Surface the hold/resume state on the dashboard at once — tick() only
	// rebuilds the session view at 1 Hz, so without this the change would lag.
	refreshSessionSnapshot();
}

std::string RequestsHandler::buildMediaSdp(const std::string& serverIp, int rtpPort, bool sendRecv)
{
	// The server's OWN SDP offer/answer. PCMU (PT 0) only — matches enforceG711()/the
	// codec the rest of the PBX speaks. CRLF line endings throughout so Content-Length
	// (computed by the caller via syncContentLength()) matches the wire bytes exactly.
	//
	// Direction:
	//   sendonly (default) — the 440 tone beachhead: the server streams to the caller
	//                        and ignores any media the caller sends back.
	//   sendrecv           — the 3CX media bridge: the handset must BOTH send its audio
	//                        (forwarded to 3CX over the POST stream) AND receive 3CX's
	//                        audio. rtpPort here MUST be the bridge receiver's port.
	std::string s;
	s += "v=0\r\n";
	s += "o=- 0 0 IN IP4 " + serverIp + "\r\n";
	s += "s=pocketdial-media\r\n";
	s += "c=IN IP4 " + serverIp + "\r\n";
	s += "t=0 0\r\n";
	s += "m=audio " + std::to_string(rtpPort) + " RTP/AVP 0\r\n";
	s += "a=rtpmap:0 PCMU/8000\r\n";
	s += sendRecv ? "a=sendrecv\r\n" : "a=sendonly\r\n";
	return s;
}

bool RequestsHandler::parseCallerRtp(const std::shared_ptr<SipMessage>& invite,
	std::string& outIp, uint16_t& outPort)
{
	// Port: the m=audio port from the caller's offered SDP (0 if absent/invalid).
	int port = 0;
	if (invite && invite->hasSdp())
	{
		auto* sdp = static_cast<SipSdpMessage*>(invite.get());
		port = sdp->getRtpPort();
	}
	if (port <= 0 || port > 65535)
	{
		return false;
	}
	outPort = static_cast<uint16_t>(port);

	// IP: prefer the SDP c= line ("c=IN IP4 <addr>"); fall back to the INVITE source
	// IP (handles phones that put 0.0.0.0 or a private/NAT addr in c=).
	outIp.clear();
	if (invite && invite->hasSdp())
	{
		auto* sdp = static_cast<SipSdpMessage*>(invite.get());
		std::string_view c = sdp->getConnectionInformation();   // "c=IN IP4 1.2.3.4"
		size_t ip4 = c.find("IP4 ");
		if (ip4 != std::string_view::npos)
		{
			size_t start = ip4 + 4;
			while (start < c.size() && std::isspace(static_cast<unsigned char>(c[start]))) ++start;
			size_t end = start;
			while (end < c.size() && (std::isdigit(static_cast<unsigned char>(c[end])) || c[end] == '.')) ++end;
			std::string candidate(c.substr(start, end - start));
			if (!candidate.empty() && candidate != "0.0.0.0")
			{
				outIp = candidate;
			}
		}
	}
	if (outIp.empty() && invite)
	{
		char ipBuf[INET_ADDRSTRLEN]{};
		sockaddr_in src = invite->getSource();
		inet_ntop(AF_INET, &src.sin_addr, ipBuf, sizeof(ipBuf));
		outIp = ipBuf;
	}
	return !outIp.empty();
}

void RequestsHandler::onMediaInvite(std::shared_ptr<SipMessage> data,
	const std::shared_ptr<SipClient>& caller)
{
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;

	// Single-stream cap: a 2nd dial of 440 while a stream is live is rejected so the
	// one media slot/socket/task is never double-booked (degrade gracefully).
	if (_rtpSender.isActive())
	{
		auto busy = getMessageFromPool(data->toString(), data->getSource());
		busy->setHeader("SIP/2.0 486 Busy Here");
		busy->clearBody();
		busy->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		busy->setContact(buildContact("440"));
		_outbox.emplace_back(data->getSource(), std::move(busy));
		queueLog("440 media: busy (one stream max), rejected " + std::string(data->getFromNumber()));
		return;
	}

	// Resolve where to stream: caller's RTP addr:port (c= line + m= port, src fallback).
	std::string destIp;
	uint16_t destPort = 0;
	if (!parseCallerRtp(data, destIp, destPort))
	{
		auto bad = getMessageFromPool(data->toString(), data->getSource());
		bad->setHeader(SipMessageTypes::BAD_REQUEST);
		bad->clearBody();
		bad->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		bad->setContact(buildContact("440"));
		_outbox.emplace_back(data->getSource(), std::move(bad));
		queueLog("440 media: no usable RTP destination in INVITE from "
			+ std::string(data->getFromNumber()), true);
		return;
	}

	// Build the 200 OK carrying the SERVER's own SDP. We rebuild the message body
	// directly (there is no generic body setter): take the INVITE clone, strip its
	// body, then append our SDP and the SDP Content-Type, and resync Content-Length
	// via enforceG711()/syncContentLength() so the answer isn't dropped on UDP (the
	// 777-bug class — see tests/SipMessage_test.cpp).
	std::string toTag = IDGen::GenerateID(9);
	std::string sdpBody = buildMediaSdp(activeIp, _rtpSender.serverRtpPort());

	auto ok = buildOkWithSdp(data, activeIp, toTag, sdpBody);
	dumpWire("[media] 200 OK ->", ok);
	_outbox.emplace_back(data->getSource(), std::move(ok));

	// Track a Session so the dashboard shows the call and CDR is recorded on teardown.
	auto virtualClient = allocateVirtualPeer("440", data->getSource());
	auto newSession = allocateSession(std::string(data->getCallID()), caller);
	if (newSession)
	{
		newSession->setDest(virtualClient);
		_sessions.emplace(data->getCallID(), newSession);
		newSession->setState(Session::State::Connected);
	}

	// Start the RTP tone stream to the caller. If it fails to start (socket/task),
	// we have already sent 200 OK; the caller's later BYE/CANCEL tears the session
	// down. Log it so the failure is visible.
	if (_rtpSender.start(destIp, destPort, std::string(data->getCallID())))
	{
		queueLog("440 media: streaming tone to " + destIp + ":" + std::to_string(destPort)
			+ " (callID=" + std::string(data->getCallID()) + ")");
	}
	else
	{
		queueLog("440 media: RTP stream failed to start to " + destIp + ":"
			+ std::to_string(destPort), true);
	}
}

void RequestsHandler::onTrying(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isBroadcast())
	{
		return;
	}
	// Inbound anchor leg (server is UAC): the handset's 100 Trying is a provisional to
	// us — swallow it rather than echoing it back at the handset (see onRinging).
	if (session.has_value() && session.value()->isAnchorInbound())
	{
		return;
	}
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onRinging(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isBroadcast())
	{
		return;
	}
	// Inbound anchor leg: the server is the UAC, so the handset's 180 is a provisional
	// to US — there is no upstream caller to relay it to. Swallow it (the upstream PSTN
	// leg already hears ringback); forwarding it would echo a 180 back at the handset.
	if (session.has_value() && session.value()->isAnchorInbound())
	{
		return;
	}
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onBusy(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isBroadcast())
	{
		// Hunt group: a busy member means advance to the next one (the timer is
		// disarmed inside huntRingNext). If the list is exhausted, fail to caller.
		if (session.value()->isHunt())
		{
			if (session.value()->getState() == Session::State::Invited && !huntRingNext(session.value()))
			{
				endHandle(session.value()->getSrc()->getNumber(), data);
				endCall(data->getCallID(), session.value()->getSrc()->getNumber(),
					session.value()->getGroupExt(), "hunt group exhausted (busy)");
			}
			return;
		}

		session.value()->removePendingTarget(std::string(data->getFromNumber()));
		if (session.value()->getPendingTargets().empty() && session.value()->getState() == Session::State::Invited)
		{
			endHandle(session.value()->getSrc()->getNumber(), data);
			const std::string& gext = session.value()->getGroupExt();
			endCall(data->getCallID(), session.value()->getSrc()->getNumber(),
				gext.empty() ? "999" : gext, "all targets busy");
		}
		return;
	}

	// Inbound ring-all: one forked extension is busy. ACK its 486 and drop it from the ring
	// set (NOT a per-extension call-forward — the PSTN call rings the others). Fail the whole
	// inbound call only if this was the last ringing leg and none answered.
	if (session.has_value() && session.value()->isAnchorInbound())
	{
		auto s = session.value();
		ackInboundFinal(s, data);
		if (s->getState() != Session::State::Connected)
		{
			s->removePendingTarget(std::string(data->getToNumber()));
			if (s->getPendingTargets().empty())
			{
				asyncDropCall(s->getAnchorParticipantId());
				endCall(std::string(data->getCallID()), s->getAnchorParticipantId(), "",
					"inbound all busy/declined");
			}
		}
		return;
	}

	// Call Forward Busy (CFB): if the busy callee has an on-busy forward target,
	// swallow the 486 and redirect the call there instead of failing the caller.
	if (session.has_value())
	{
		std::string busyExt(data->getFromNumber());
		std::string cfb = getForwardTarget(busyExt, "busy");
		if (!cfb.empty() && cfb != busyExt)
		{
			auto inviteMsg = session.value()->getInviteMessage();
			auto src = session.value()->getSrc();
			if (inviteMsg && src)
			{
				queueLog("CFB: " + busyExt + " busy, forwarding -> " + cfb);
				std::string callID(data->getCallID());
				// Tear down the busy leg's session, then start a fresh leg to the
				// forward target reusing the retained original INVITE.
				endCall(callID, src->getNumber(), busyExt, "forwarded on busy");
				if (redirectInvite(inviteMsg, src, cfb))
				{
					return;
				}
			}
		}
	}

	setCallState(data->getCallID(), Session::State::Busy);
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onUnavailable(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isBroadcast())
	{
		// Hunt group: treat unavailable like busy — advance to the next member.
		if (session.value()->isHunt())
		{
			if (session.value()->getState() == Session::State::Invited && !huntRingNext(session.value()))
			{
				endHandle(session.value()->getSrc()->getNumber(), data);
				endCall(data->getCallID(), session.value()->getSrc()->getNumber(),
					session.value()->getGroupExt(), "hunt group exhausted (unavailable)");
			}
			return;
		}

		session.value()->removePendingTarget(std::string(data->getFromNumber()));
		if (session.value()->getPendingTargets().empty() && session.value()->getState() == Session::State::Invited)
		{
			endHandle(session.value()->getSrc()->getNumber(), data);
			const std::string& gext = session.value()->getGroupExt();
			endCall(data->getCallID(), session.value()->getSrc()->getNumber(),
				gext.empty() ? "999" : gext, "all targets unavailable");
		}
		return;
	}
	// Inbound ring-all: one forked extension is unavailable (DND / out of range). ACK its 480
	// and drop it from the ring set; fail the whole inbound call only if it was the last
	// ringing leg and none answered.
	if (session.has_value() && session.value()->isAnchorInbound())
	{
		auto s = session.value();
		ackInboundFinal(s, data);
		if (s->getState() != Session::State::Connected)
		{
			s->removePendingTarget(std::string(data->getToNumber()));
			if (s->getPendingTargets().empty())
			{
				asyncDropCall(s->getAnchorParticipantId());
				endCall(std::string(data->getCallID()), s->getAnchorParticipantId(), "",
					"inbound all unavailable");
			}
		}
		return;
	}
	setCallState(data->getCallID(), Session::State::Unavailable);
	endHandle(data->getFromNumber(), data);
}

// Park header helpers (defined with the park methods below); forward-declared so
// onBye() can build the bridged-peer BYE from the stored dialog headers.
static std::string parkTagOf(std::string_view header);
static std::string stripHeaderName(std::string_view fullLine);

// Dialog-source binding for in-dialog teardown (issue #46). The registrar does not
// digest-challenge BYE/CANCEL (only REGISTER in secure mode — see admitSecure /
// THREAT_MODEL §9.1), so without this check any peer that can guess or sniff a live
// Call-ID on the open SoftAP could forge a BYE and drop an arbitrary call (incl. a
// PSTN-anchor leg). As a link-independent backstop we require an in-dialog teardown
// to ORIGINATE from the IP of one of the call's two real phone legs. This is the
// "source-IP binding" interim fix the threat model names; it is not full per-request
// digest, but it removes the off-path attacker (different IP) entirely and is free
// (pure in-memory address compare, no I/O under the lock).
bool RequestsHandler::isDialogSourceAuthorized(const std::shared_ptr<Session>& session,
	const sockaddr_in& source) const
{
	if (!session)
	{
		// No session yet (out-of-dialog / unknown Call-ID). Don't gate here — the
		// caller's per-extension/virtual handlers decide; a forged BYE for a
		// non-existent dialog is harmless (nothing to tear down).
		return true;
	}

	auto src  = session->getSrc();
	auto dest = session->getDest();

	// Fail-open for any dialog whose teardown we cannot meaningfully bind to a phone
	// IP: an anchor/inbound-anchor leg (one side is the upstream trunk, not a LAN
	// phone), or a half-set-up dialog missing a leg. Those paths have their own
	// teardown owners (anchor drop, virtual-ext handlers) and must not be broken.
	if (session->isAnchor() || session->isAnchorInbound() || !src || !dest)
	{
		return true;
	}

	// Compare source IP only (port-agnostic): a phone may send the BYE from the same
	// contact IP but a different ephemeral UDP port than the original INVITE.
	const uint32_t fromIp = source.sin_addr.s_addr;
	return fromIp == src->getAddress().sin_addr.s_addr ||
	       fromIp == dest->getAddress().sin_addr.s_addr;
}

void RequestsHandler::onBye(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	std::string destNumber(data->getToNumber());

	// Issue #46: reject an off-path forged teardown. A BYE for an established
	// two-phone dialog must originate from one of the call's leg IPs; otherwise a
	// peer that only guessed/sniffed the Call-ID could drop someone else's call.
	// Virtual/anchor/half-set-up dialogs fail open (handled by their own teardown
	// owners — see isDialogSourceAuthorized). 403 + return: leave the dialog up.
	if (session.has_value() &&
		!isDialogSourceAuthorized(session.value(), data->getSource()))
	{
		queueLog("BYE for Call-ID " + std::string(data->getCallID()) +
			" rejected: source not a dialog leg (spoofed teardown)", true);
		sendForbidden(data, "Forbidden");
		return;
	}

	// Call parking: a BYE whose To is an orbit ("70x"), or any leg linked to a
	// bridged peer. Always 200 OK the BYE; if the leg has a retrieved/rung-back
	// peer, relay a server BYE to that phone too, then end both dialogs. A still-
	// parked leg just frees its orbit (endCall -> freeParkSlot).
	if (parkOrbitIndex(destNumber) >= 0 ||
		(session.has_value() && !session.value()->getPeerCallID().empty()))
	{
		const std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader(SipMessageTypes::OK);
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));

		if (session.has_value() && !session.value()->getPeerCallID().empty())
		{
			const std::string peerId = session.value()->getPeerCallID();
			if (auto peer = getSession(peerId); peer.has_value())
			{
				auto peerSess = peer.value();
				auto peerInvite = peerSess->getInviteMessage();
				auto peerClient = peerSess->getSrc();
				if (peerInvite && peerClient)
				{
					std::string fromHeader, toHeader;
					if (peerSess->isParkUac())
					{
						// Server originated this peer leg (ring-back UAC): the stored
						// message is the parker's 200 OK, whose From is us and To is the
						// parker — reuse them directly (roles are inverted vs a UAS leg).
						fromHeader = stripHeaderName(peerInvite->getFrom());
						toHeader = stripHeaderName(peerInvite->getTo());
					}
					else
					{
						// Normal UAS leg: we minted the To-tag (localTag); the phone's
						// tag is the From-tag of the INVITE it sent us.
						const std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
						const std::string peerTag = parkTagOf(peerInvite->getFrom());
						fromHeader = "<sip:" + std::string(peerInvite->getToNumber()) +
							"@" + srcIpPort + ">;tag=" + peerSess->getLocalTag();
						toHeader = "<sip:" + peerClient->getNumber() + "@" + activeIp +
							">;tag=" + peerTag;
					}
					auto bye = buildServerBye(peerClient->getNumber(), peerClient->getAddress(),
						stripHeaderName(peerId), fromHeader, toHeader);
					if (bye) _outbox.emplace_back(peerClient->getAddress(), std::move(bye));
				}
				endCall(peerId, peerClient ? peerClient->getNumber() : std::string(),
					destNumber, "park bridge peer BYE");
			}
		}
		endCall(data->getCallID(), data->getFromNumber(), destNumber, "park BYE");
		refreshParkSnapshot();
		return;
	}

	if (destNumber == "777")
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	if (destNumber == "440")
	{
		// Media beachhead teardown: stop the RTP tone stream (only if it owns this
		// Call-ID), 200 OK the BYE, and end the session. Stream stop is idempotent.
		_rtpSender.stop(std::string(data->getCallID()));
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	if (destNumber == "999" || isPageZoneDialog(destNumber))
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));

		if (session.has_value())
		{
			auto answeringClient = session.value()->getDest();
			if (answeringClient)
			{
				auto byeFork = getMessageFromPool(data->toString(), data->getSource());
				std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);

				char ipBuf[INET_ADDRSTRLEN]{};
				inet_ntop(AF_INET, &answeringClient->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
				std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(answeringClient->getAddress().sin_port));

				byeFork->setHeader("BYE sip:" + answeringClient->getNumber() + "@" + targetIpPort + " SIP/2.0");

				std::string originalTo(data->getTo());
				std::string newTo = "To: <sip:" + answeringClient->getNumber() + "@" + serverIpPort + ">";
				size_t tagPos = originalTo.find(";tag=");
				if (tagPos != std::string::npos)
				{
					newTo += originalTo.substr(tagPos);
				}
				byeFork->setTo(newTo);

				_outbox.emplace_back(answeringClient->getAddress(), std::move(byeFork));
			}
		}
		endCall(data->getCallID(), data->getFromNumber(), destNumber);
		return;
	}

	if (session.has_value() && session.value()->isAnchor())
	{
		_mediaBridge.stopBridge();
		asyncDropCall(session.value()->getAnchorParticipantId());   // drop the PSTN leg by id (REST)
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader(SipMessageTypes::OK);
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		endCall(data->getCallID(), data->getFromNumber(), destNumber, "handset BYE");
		return;
	}

	setCallState(data->getCallID(), Session::State::Bye);
	endHandle(data->getToNumber(), data);
}

void RequestsHandler::onOk(std::shared_ptr<SipMessage> data)
{
	if (data->getCSeq().find("OPTIONS") != std::string::npos)
	{
		return;
	}

	// Register-beep dialog (server-originated UAC). These have NO Session — they are
	// tracked in _beepDialogs by Call-ID. A 200 OK to our INVITE means the phone
	// auto-answered (the tone played): ACK it, then BYE to end the call. A 200 OK to
	// our BYE just frees the slot. Recognised before the normal session lookup.
	if (BeepDialog* bd = findBeepByCallID(data->getCallID()))
	{
		std::string cseq(data->getCSeq());
		if (bd->state == BeepState::AwaitingInviteOk &&
			cseq.find(SipMessageTypes::INVITE) != std::string::npos)
		{
			auto ack = buildBeepAck(data);
			if (ack) _outbox.emplace_back(bd->addr, std::move(ack));
			auto bye = buildBeepBye(data);
			if (bye) _outbox.emplace_back(bd->addr, std::move(bye));
			bd->state    = BeepState::AwaitingByeOk;
			// Re-arm the deadline so a phone that never 200s our BYE still frees its
			// slot from tick() rather than lingering until the original INVITE timeout.
			bd->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			queueLog("Register beep: answered by " + bd->ext + ", ACK+BYE sent");
		}
		else if (cseq.find(SipMessageTypes::BYE) != std::string::npos)
		{
			*bd = BeepDialog{};   // BYE acknowledged: dialog fully torn down, free slot
		}
		return;
	}

	// Park dialogs (server-originated re-INVITE ACKs + ring-back answers). Like the
	// beep table these are recognised by Call-ID before the normal session lookup.
	if (handleParkOk(data))
	{
		return;
	}

	auto session = getSession(data->getCallID());
	if (session.has_value())
	{
		// Inbound anchor leg: the server is the UAC, so a 200 OK to our INVITE is the
		// handset ANSWERING — drive the media bridge + ACK + upstream answer here, never
		// the generic B2BUA forward below (which assumes the handset is the caller).
		if (session.value()->isAnchorInbound() &&
		    data->getCSeq().find(SipMessageTypes::INVITE) != std::string::npos)
		{
			onInboundAnchorOk(data, session.value());
			return;
		}

		if (session.value()->getState() == Session::State::Cancel)
		{
			endHandle(data->getFromNumber(), data);
			return;
		}

		if (data->getCSeq().find(SipMessageTypes::INVITE) != std::string::npos)
		{
			// Re-INVITE answer (hold/resume): a 200 OK with an INVITE CSeq for a
			// session that is ALREADY established is the peer answering the relayed
			// re-INVITE, not the initial call answer. Relay it UNTOUCHED to the
			// opposite leg (address-compared) and keep the session state set by
			// onReinvite() — do NOT re-run the connect transition.
			{
				const auto st = session.value()->getState();
				if ((st == Session::State::Connected || st == Session::State::Held) &&
					!session.value()->isBroadcast())
				{
					auto legSrc = session.value()->getSrc();
					auto legDest = session.value()->getDest();
					if (legSrc && legDest)
					{
						std::shared_ptr<SipClient> peer;
						if (sameAddress(data->getSource(), legSrc->getAddress()))
						{
							peer = legDest;
						}
						else if (sameAddress(data->getSource(), legDest->getAddress()))
						{
							peer = legSrc;
						}
						if (peer)
						{
							_outbox.emplace_back(peer->getAddress(), data);
							return;
						}
					}
				}
			}

			if (session.value()->isBroadcast())
			{
				if (session.value()->getState() != Session::State::Connected)
				{
					auto clientOpt = findClientByAddress(data->getSource());
					if (!clientOpt.has_value())
					{
						return;
					}
					auto answeringClient = clientOpt.value();

					SipSdpMessage* sdpMessage = nullptr;
					if (data->hasSdp())
					{
						sdpMessage = static_cast<SipSdpMessage*>(data.get());
					}
					if (!sdpMessage)
					{
						queueLog("Couldn't get SDP from: " + answeringClient->getNumber() + "'s broadcast OK message.", true);
						return;
					}

					session->get()->setDest(answeringClient);
					session->get()->setState(Session::State::Connected);
					session->get()->clearRingTimer();   // hunt answered: disarm timeout

					auto inviteMsg = session.value()->getInviteMessage();

					auto response = getMessageFromPool(data->toString(), data->getSource());
					response->setContact(buildContact(answeringClient->getNumber()));

					if (inviteMsg)
					{
						std::string originalTo(inviteMsg->getTo());
						std::string bTo(data->getTo());
						size_t tagPos = bTo.find(";tag=");
						if (tagPos != std::string::npos)
						{
							originalTo += bTo.substr(tagPos);
						}
						response->setTo(originalTo);
					}

					response->enforceG711();
					endHandle(session.value()->getSrc()->getNumber(), std::move(response));

					if (inviteMsg)
					{
						std::string originalCSeq(inviteMsg->getCSeq());
						size_t invitePos = originalCSeq.find("INVITE");
						if (invitePos != std::string::npos)
						{
							originalCSeq.replace(invitePos, 6, "CANCEL");
						}

						for (const auto& target : session.value()->getPendingTargets())
						{
							if (target->getNumber() != answeringClient->getNumber())
							{
								auto cancelMsg = getMessageFromPool(inviteMsg->toString(), inviteMsg->getSource());
								char ipBuf[INET_ADDRSTRLEN]{};
								inet_ntop(AF_INET, &target->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
								std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(target->getAddress().sin_port));

								cancelMsg->setHeader("CANCEL sip:" + target->getNumber() + "@" + targetIpPort + " SIP/2.0");
								cancelMsg->setCSeq(originalCSeq);
								_outbox.emplace_back(target->getAddress(), std::move(cancelMsg));
							}
						}
					}
				}
				return;
			}

			auto client = findClient(data->getToNumber());
			if (!client.has_value())
			{
				return;
			}

			SipSdpMessage* sdpMessage = nullptr;
			if (data->hasSdp())
			{
				sdpMessage = static_cast<SipSdpMessage*>(data.get());
			}
			if (!sdpMessage) 
			{
				queueLog("Couldn't get SDP from: " + client.value()->getNumber() + "'s OK message.", true);
				std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
				responseObj->setHeader(SipMessageTypes::BAD_REQUEST);
				responseObj->clearBody();
				responseObj->setContact(buildContact(data->getToNumber()));
				endHandle(data->getToNumber(), responseObj);
				endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), "SDP parse error.");
				return;
			}
			session->get()->setDest(client.value());
			session->get()->setState(Session::State::Connected);
			session->get()->clearRingTimer();   // answered: disarm any CFNA timeout
			auto response = getMessageFromPool(data->toString(), data->getSource());
			response->setContact(buildContact(data->getToNumber()));
			endHandle(data->getFromNumber(), std::move(response));
			return;
		}

		if (session.value()->getState() == Session::State::Bye)
		{
			endHandle(data->getFromNumber(), data);
			endCall(data->getCallID(), data->getToNumber(), data->getFromNumber());
		}
	}
}

void RequestsHandler::onAck(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (!session.has_value())
	{
		return;
	}

	// DIAGNOSTIC: an ACK for a 3CX-routed (anchor) session means the handset
	// ACCEPTED our 200 OK and the SIP call is fully connected — so any remaining
	// silence is purely an RTP/media problem, not a SIP one.
	if (session.value()->isAnchor())
	{
		queueLog("[3CX] Handset ACK received — SIP dialog CONNECTED for " + std::string(data->getCallID()));
		// The server is the UAS for the handset leg of a WAN-anchor (trunk) call, so
		// this ACK confirms our 200 OK and is the end of the transaction — ABSORB it.
		// Falling through routes the ACK to getToNumber() (the dialed PSTN number,
		// not a registered extension) via endHandle(), which answers the ACK with a
		// 404 — illegal (you never respond to an ACK) and it makes tag-strict phones
		// (Yealink) retransmit, storming the dialog until 3CX reaps the call (~15s).
		return;
	}

	std::string destNumber(data->getToNumber());
	if (destNumber == "777")
	{
		return;
	}

	// Park/retrieve dialogs are server-terminated (the server is the UAS for the
	// parked leg); absorb the ACK rather than relaying it to the virtual orbit
	// "peer" (the caller's own address), which churns the dialog.
	if (parkOrbitIndex(destNumber) >= 0)
	{
		return;
	}

	if (destNumber == "999" || isPageZoneDialog(destNumber))
	{
		auto answeringClient = session.value()->getDest();
		if (answeringClient)
		{
			auto ackFork = getMessageFromPool(data->toString(), data->getSource());
			std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
			std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);

			char ipBuf[INET_ADDRSTRLEN]{};
			inet_ntop(AF_INET, &answeringClient->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
			std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(answeringClient->getAddress().sin_port));

			ackFork->setHeader("ACK sip:" + answeringClient->getNumber() + "@" + targetIpPort + " SIP/2.0");

			std::string originalTo(data->getTo());
			std::string newTo = "To: <sip:" + answeringClient->getNumber() + "@" + serverIpPort + ">";
			size_t tagPos = originalTo.find(";tag=");
			if (tagPos != std::string::npos)
			{
				newTo += originalTo.substr(tagPos);
			}
			ackFork->setTo(newTo);

			_outbox.emplace_back(answeringClient->getAddress(), std::move(ackFork));
		}
		return;
	}

	endHandle(data->getToNumber(), data);

	auto sessionState = session.value()->getState();
	std::string endReason;
	if (sessionState == Session::State::Busy)
	{
		endReason = std::string(data->getToNumber()) + " is busy.";
		endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), endReason);
		return;
	}

	if (sessionState == Session::State::Unavailable)
	{
		endReason = std::string(data->getToNumber()) + " is unavailable.";
		endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), endReason);
		return;
	}

	if (sessionState == Session::State::Cancel)
	{
		endReason = std::string(data->getFromNumber()) + " canceled the session.";
		endCall(data->getCallID(), data->getFromNumber(), data->getToNumber(), endReason);
		return;
	}
}

void RequestsHandler::onRefer(std::shared_ptr<SipMessage> data)
{
	// Blind (unattended) transfer, RFC 3515. The transferor (the party that holds
	// the call and pressed "transfer") sends REFER with a Refer-To header naming the
	// new target. We ack 202 Accepted, then drive a fresh INVITE from the transferor
	// to the target, and report progress with a NOTIFY (Event: refer + sipfrag body).
	// Attended transfer (Refer-To carrying a Replaces= dialog) is OUT OF SCOPE — see
	// the summary; such a REFER is treated as a blind transfer to the named target.
	auto transferorOpt = findClient(data->getFromNumber());
	if (!transferorOpt.has_value())
	{
		// Unknown transferor: reject (consistent with onInvite's 403 for non-registered).
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader("SIP/2.0 403 Forbidden");
		response->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}
	auto transferor = transferorOpt.value();

	// Pull the Refer-To header value out of the raw message and extract the target.
	std::string target;
	{
		const std::string& raw = data->toString();
		// Case-insensitive scan for a "Refer-To:" header line (no compact form in 3515).
		size_t pos = 0;
		while (pos < raw.size())
		{
			size_t lineEnd = raw.find('\n', pos);
			size_t next = (lineEnd == std::string::npos) ? raw.size() : lineEnd + 1;
			if ((raw[pos] == 'r' || raw[pos] == 'R') && next - pos >= 9)
			{
				std::string name = raw.substr(pos, 9);
				std::transform(name.begin(), name.end(), name.begin(),
					[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
				if (name == "refer-to:")
				{
					size_t valEnd = (lineEnd == std::string::npos) ? raw.size() : lineEnd;
					std::string value = raw.substr(pos + 9, valEnd - (pos + 9));
					target = pbx::parseReferToTarget(value);
					break;
				}
			}
			else if (raw[pos] == '\r' || raw[pos] == '\n')
			{
				break; // header/body boundary
			}
			pos = next;
		}
	}

	if (target.empty() || !isValidAor(target))
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader(SipMessageTypes::BAD_REQUEST);
		response->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}

	// 202 Accepted to the transferor (RFC 3515 §2.4.4).
	{
		auto accepted = getMessageFromPool(data->toString(), data->getSource());
		accepted->setHeader(SipMessageTypes::ACCEPTED);
		accepted->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		accepted->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		accepted->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
		_outbox.emplace_back(data->getSource(), std::move(accepted));
	}

	// Blind transfer drops the transferor's ORIGINAL call. Tear that leg down FIRST,
	// then drive the new INVITE — ordering is load-bearing: redirectInvite() reuses the
	// Session stored under this Call-ID, so ending the call AFTER it (the previous
	// order) erased the freshly-created transfer leg, and the target's 200 OK then
	// matched no session and the transfer silently never completed. onBusy()/tick()
	// (CFB/CFNA) end-then-redirect for exactly this reason. CDR is recorded as the
	// original leg tears down; redirectInvite() then allocates a clean session.
	std::string callID(data->getCallID());
	auto targetClient = findClient(target);

	endCall(callID, transferor->getNumber(), std::string(data->getToNumber()), "blind transfer");

	bool ok = targetClient.has_value() && redirectInvite(data, transferor, target);

	// NOTIFY the transferor with the transfer result (message/sipfrag body).
	std::string frag = ok ? "SIP/2.0 200 OK" : "SIP/2.0 404 Not Found";
	auto notify = buildReferNotify(data, transferor, frag, /*terminated=*/true);
	if (notify)
	{
		_outbox.emplace_back(transferor->getAddress(), std::move(notify));
	}

	if (!ok)
	{
		queueLog("REFER: blind transfer to " + target + " failed (target not registered)", true);
	}
	else
	{
		queueLog("REFER: blind transfer " + transferor->getNumber() + " -> " + target);
	}
}

void RequestsHandler::onMessage(std::shared_ptr<SipMessage> data)
{
	// Inbound MESSAGE hygiene (RFC 3428). Phones may send delivery receipts / IMs;
	// if we don't 200 them they retransmit. We do NOT interpret the body and the
	// server never originates a MESSAGE. Simple stateless ack, mirroring onOptions().
	auto response = getMessageFromPool(data->toString(), data->getSource());
	response->setHeader(SipMessageTypes::OK);
	response->clearBody();
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	response->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
	_outbox.emplace_back(data->getSource(), std::move(response));
}

// ── BLF/presence: SUBSCRIBE/NOTIFY dialog-event package (RFC 6665 + RFC 4235) ──

std::string RequestsHandler::parseEventPackage(const std::string& raw)
{
	// Scan header lines only (stop at the blank header/body boundary). Accept the
	// full "Event:" name and the compact form "o:" (RFC 6665 §8.2.4),
	// case-insensitively, and strip any ;parameters and surrounding whitespace.
	size_t pos = 0;
	while (pos < raw.size())
	{
		size_t nl = raw.find('\n', pos);
		size_t lineEnd = (nl == std::string::npos) ? raw.size() : nl;
		size_t next = (nl == std::string::npos) ? raw.size() : nl + 1;
		if (raw[pos] == '\r' || raw[pos] == '\n') break;   // end of headers

		size_t colon = raw.find(':', pos);
		if (colon != std::string::npos && colon < lineEnd)
		{
			std::string name = raw.substr(pos, colon - pos);
			std::transform(name.begin(), name.end(), name.begin(),
				[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
			if (name == "event" || name == "o")
			{
				size_t vBegin = colon + 1;
				size_t vEnd = lineEnd;
				// Trim trailing \r and whitespace; cut at the first ';' parameter.
				std::string val = raw.substr(vBegin, vEnd - vBegin);
				size_t semi = val.find(';');
				if (semi != std::string::npos) val.erase(semi);
				size_t b = val.find_first_not_of(" \t\r");
				size_t e = val.find_last_not_of(" \t\r");
				if (b == std::string::npos) return "";
				return val.substr(b, e - b + 1);
			}
		}
		pos = next;
	}
	return "";
}

std::string RequestsHandler::buildDialogInfoXml(const std::string& entity, unsigned version,
	const std::string& dialogId, const std::string& state, const std::string& direction)
{
	// Minimal RFC 4235 document, always state="full": each NOTIFY carries the
	// complete (zero-or-one element) dialog set, so watchers never need to merge
	// partials. An empty `state` omits the <dialog> element entirely — the
	// canonical "idle lamp" body that Yealink/Grandstream BLF keys expect.
	std::ostringstream xml;
	xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
	    << "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\""
	    << version << "\" state=\"full\" entity=\"" << entity << "\">\r\n";
	if (!state.empty())
	{
		xml << "<dialog id=\"" << dialogId << "\"";
		if (!direction.empty()) xml << " direction=\"" << direction << "\"";
		xml << "><state>" << state << "</state></dialog>\r\n";
	}
	xml << "</dialog-info>\r\n";
	return xml.str();
}

std::string RequestsHandler::computeDialogState(const std::string& targetAor,
	std::string& outDirection, std::string& outDialogId) const
{
	// Map the target's busiest session onto the RFC 4235 dialog state ladder.
	// Confirmed (in-call) outranks early/trying (ringing/originating); anything
	// else is idle (empty state — no dialog element).
	std::string best;          // ""=idle < trying < early < confirmed
	auto rank = [](const std::string& s) -> int {
		if (s == "confirmed") return 3;
		if (s == "early")     return 2;
		if (s == "trying")    return 1;
		return 0;
	};
	for (const auto& [callID, session] : _sessions)
	{
		bool isSrc  = session->getSrc()  && session->getSrc()->getNumber()  == targetAor;
		bool isDest = session->getDest() && session->getDest()->getNumber() == targetAor;
		if (!isSrc && !isDest) continue;

		std::string state, dir;
		switch (session->getState())
		{
			case Session::State::Invited:
				state = isDest ? "early" : "trying";   // ringing vs originating
				dir   = isDest ? "recipient" : "initiator";
				break;
			case Session::State::Connected:
			case Session::State::Held:
				// RFC 4235 §4: a held call is an *established* dialog — its media is
				// merely on hold (sendonly/recvonly/inactive), so the dialog state stays
				// "confirmed" and the watcher's BLF lamp must remain lit. Without the
				// Held case this fell through to default/idle and darkened the lamp
				// mid-hold (#53), even though the line is still busy.
				state = "confirmed";
				dir   = isSrc ? "initiator" : "recipient";
				break;
			default:
				continue;   // Busy/Cancel/Bye/etc: dialog is (about to be) gone
		}
		if (rank(state) > rank(best))
		{
			best = state;
			outDirection = dir;
			outDialogId  = callID;
		}
	}
	if (best.empty()) { outDirection.clear(); outDialogId.clear(); }
	return best;
}

std::shared_ptr<SipMessage> RequestsHandler::buildDialogNotify(DialogSubscription& sub,
	const std::string& state, const std::string& direction, const std::string& dialogId,
	bool terminated, const char* termReason)
{
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &sub.addr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(sub.addr.sin_port));
	std::string branch = "z9hG4bK" + IDGen::GenerateID(12);

	std::string entity = "sip:" + sub.targetAor + "@" + srcIpPort;
	std::string body = buildDialogInfoXml(entity, sub.version, dialogId, state, direction);

	int remaining = 0;
	if (!terminated)
	{
		auto now = std::chrono::steady_clock::now();
		remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
			sub.deadline - now).count());
		if (remaining < 0) remaining = 0;
	}
	std::string subState = terminated
		? ("terminated;reason=" + std::string(termReason))
		: ("active;expires=" + std::to_string(remaining));

	// NOTIFY runs inside the subscription dialog: our To-tag becomes the From,
	// the watcher's From becomes the To (RFC 6665 §4.4.1 — roles swap).
	std::string fromHdr = sub.subTo;       // "To: <...>;tag=ours"
	std::string toHdr   = sub.watcherFrom; // "From: <...>;tag=theirs"
	// Re-label the header names.
	auto stripName = [](const std::string& h) {
		size_t c = h.find(':');
		return (c == std::string::npos) ? h : h.substr(c + 1);
	};

	std::ostringstream ss;
	ss << "NOTIFY sip:" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "From:" << stripName(fromHdr) << "\r\n"
	   << "To:" << stripName(toHdr) << "\r\n"
	   << "Call-ID: " << sub.callId << "\r\n"
	   << "CSeq: " << sub.cseq++ << " NOTIFY\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Event: dialog\r\n"
	   << "Subscription-State: " << subState << "\r\n"
	   << "Contact: <sip:" << sub.targetAor << "@" << srcIpPort << ">\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Type: application/dialog-info+xml\r\n"
	   << "Content-Length: " << body.size() << "\r\n\r\n"
	   << body;

	return getMessageFromPool(ss.str(), sub.addr);
}

void RequestsHandler::onSubscribe(std::shared_ptr<SipMessage> data)
{
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;

	// 1. Event-package gate: only the RFC 4235 "dialog" package is implemented.
	std::string pkg = parseEventPackage(data->toString());
	if (pkg != "dialog")
	{
		auto resp = getMessageFromPool(data->toString(), data->getSource());
		resp->setHeader(SipMessageTypes::BAD_EVENT);
		resp->clearBody();
		resp->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		resp->addHeader("Allow-Events", "dialog");
		_outbox.emplace_back(data->getSource(), std::move(resp));
		return;
	}

	// 2. Watched-target validation: the To user-part must be a well-formed AOR.
	std::string target(data->getToNumber());
	if (!isValidAor(target))
	{
		auto resp = getMessageFromPool(data->toString(), data->getSource());
		resp->setHeader(SipMessageTypes::BAD_REQUEST);
		resp->clearBody();
		resp->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(resp));
		return;
	}

	const int expires = parseRequestedExpires(data);
	// getCallID() returns the full header line ("Call-ID: x" / compact "i: x");
	// normalize to the bare value so the stored id is form-independent and can be
	// re-emitted verbatim in the NOTIFY's own Call-ID header.
	std::string callId(data->getCallID());
	{
		size_t c = callId.find(':');
		if (c != std::string::npos) callId.erase(0, c + 1);
		size_t b = callId.find_first_not_of(" \t");
		size_t e = callId.find_last_not_of(" \t\r");
		callId = (b == std::string::npos) ? "" : callId.substr(b, e - b + 1);
	}

	// 3. Refresh / unsubscribe: an existing subscription is matched by Call-ID.
	DialogSubscription* sub = nullptr;
	for (auto& s : _subscriptions)
	{
		if (s.used && s.callId == callId) { sub = &s; break; }
	}

	if (sub == nullptr && expires > 0)
	{
		// New subscription: take the first free fixed slot; 503 on exhaustion
		// (graceful degradation, mirroring the client/session pools).
		for (auto& s : _subscriptions)
		{
			if (!s.used) { sub = &s; break; }
		}
		if (sub == nullptr)
		{
			auto resp = getMessageFromPool(data->toString(), data->getSource());
			resp->setHeader("SIP/2.0 503 Service Unavailable");
			resp->clearBody();
			resp->setVia(std::string(data->getVia()) + ";received=" + activeIp);
			_outbox.emplace_back(data->getSource(), std::move(resp));
			queueLog("BLF: subscription pool exhausted, 503 to watcher of " + target, true);
			return;
		}
		*sub = DialogSubscription{};
		sub->used        = true;
		sub->callId      = callId;
		sub->watcherFrom = std::string(data->getFrom());
		sub->subTo       = std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9);
		sub->targetAor   = target;
		queueLog("BLF: new subscription, watcher " + std::string(data->getFromNumber())
			+ " -> target " + target);
	}

	// 4. 202 Accepted (RFC 6665 §4.2.1) — sent before the NOTIFY.
	{
		auto resp = getMessageFromPool(data->toString(), data->getSource());
		resp->setHeader(SipMessageTypes::ACCEPTED);
		resp->clearBody();
		resp->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		if (sub) resp->setTo(sub->subTo);
		else     resp->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
		resp->setContact(buildContact(target));
		resp->addHeader("Expires", std::to_string(expires));
		_outbox.emplace_back(data->getSource(), std::move(resp));
	}

	if (sub == nullptr)
	{
		// Expires: 0 for a subscription we don't hold — nothing to terminate.
		return;
	}

	sub->addr       = data->getSource();
	sub->expiresSec = expires;
	sub->deadline   = std::chrono::steady_clock::now() + std::chrono::seconds(expires);

	// 5. Immediate NOTIFY (RFC 6665 §4.2.1.4): full current state, or the terminal
	//    NOTIFY when this SUBSCRIBE is an unsubscribe (Expires: 0).
	std::string dir, dialogId;
	std::string state = computeDialogState(target, dir, dialogId);
	const bool terminating = (expires == 0);
	auto notify = buildDialogNotify(*sub, state, dir, dialogId,
		terminating, "noresource");
	_outbox.emplace_back(sub->addr, std::move(notify));
	sub->lastState = state + "|" + dir + "|" + dialogId;
	sub->version++;

	if (terminating)
	{
		queueLog("BLF: unsubscribe, watcher of " + target + " released");
		*sub = DialogSubscription{};   // free the slot
	}
}

void RequestsHandler::refreshSubscriptions()
{
	for (auto& sub : _subscriptions)
	{
		if (!sub.used) continue;
		std::string dir, dialogId;
		std::string state = computeDialogState(sub.targetAor, dir, dialogId);
		std::string token = state + "|" + dir + "|" + dialogId;
		if (token == sub.lastState) continue;
		auto notify = buildDialogNotify(sub, state, dir, dialogId, false, "");
		_outbox.emplace_back(sub.addr, std::move(notify));
		sub.lastState = token;
		sub.version++;
	}
}

void RequestsHandler::sweepSubscriptions()
{
	auto now = std::chrono::steady_clock::now();
	for (auto& sub : _subscriptions)
	{
		if (!sub.used || now < sub.deadline) continue;
		// RFC 6665 §3.3.4: expiry is announced with a terminal NOTIFY.
		std::string dir, dialogId;
		std::string state = computeDialogState(sub.targetAor, dir, dialogId);
		auto notify = buildDialogNotify(sub, state, dir, dialogId, true, "timeout");
		_outbox.emplace_back(sub.addr, std::move(notify));
		queueLog("BLF: subscription to " + sub.targetAor + " expired");
		sub = DialogSubscription{};   // free the slot
	}
}

void RequestsHandler::buildInviteFork(const std::shared_ptr<SipMessage>& invite,
	const std::shared_ptr<SipClient>& caller,
	const std::shared_ptr<SipClient>& target,
	bool intercom)
{
	auto inviteFork = getMessageFromPool(invite->toString(), invite->getSource());
	inviteFork->setContact(buildContact(caller->getNumber()));

	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &target->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
	std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(target->getAddress().sin_port));
	std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);

	inviteFork->setHeader("INVITE sip:" + target->getNumber() + "@" + targetIpPort + " SIP/2.0");
	inviteFork->setTo("To: <sip:" + target->getNumber() + "@" + serverIpPort + ">");

	if (intercom)
	{
		// Auto-answer / intercom headers — used by 999 all-page only. A ring group
		// omits these so members ring normally (the caller can be picked up by hand).
		inviteFork->addHeader("Call-Info", "<sip:any>;answer-after=0");
		inviteFork->addHeader("Alert-Info", "info=alert-autoanswer");
		inviteFork->addHeader("Alert-Info", "answer-after=0");
		inviteFork->addHeader("Alert-Info", "intercom=true");
		inviteFork->addHeader("P-Auto-Answer", "normal");
	}
	inviteFork->enforceG711();
	_outbox.emplace_back(target->getAddress(), std::move(inviteFork));
}

void RequestsHandler::startBroadcastFork(std::shared_ptr<SipMessage> invite,
	std::shared_ptr<SipClient> caller,
	const std::vector<std::shared_ptr<SipClient>>& targets,
	bool intercom)
{
	// Shared fan-out core: build the broadcast Session, send 180 Ringing to the
	// caller, then one forked INVITE per target. First answer wins; onOk() cancels
	// the losers (it walks getPendingTargets()). Used by 999 (intercom=true) and
	// ring-all groups (intercom=false).
	auto newSession = allocateSession(std::string(invite->getCallID()), caller);
	if (!newSession)
	{
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(invite->toString(), invite->getSource());
		responseObj->setHeader("SIP/2.0 503 Service Unavailable");
		responseObj->clearBody();
		responseObj->setContact(buildContact(caller->getNumber()));
		_outbox.emplace_back(invite->getSource(), std::move(responseObj));
		return;
	}
	newSession->setBroadcast(true);
	newSession->setPendingTargets(targets);
	newSession->setInviteMessage(invite);
	// Record the dialled group/zone extension (999, a 6xx ring group or a 98x
	// paging zone) so CDR rows and end reasons report what was actually dialled
	// instead of hardcoding "999".
	newSession->setGroupExt(std::string(invite->getToNumber()));
	_sessions.emplace(invite->getCallID(), newSession);

	// The caller's dialog Contact carries whatever virtual extension was dialled
	// (999, zone ext or group ext) — it is the To-number in every fork path.
	std::string contactExt(invite->getToNumber());

	auto ringing = getMessageFromPool(invite->toString(), invite->getSource());
	ringing->setHeader("SIP/2.0 180 Ringing");
	ringing->clearBody();
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	ringing->setVia(std::string(invite->getVia()) + ";received=" + activeIp);
	ringing->setTo(std::string(invite->getTo()) + ";tag=" + IDGen::GenerateID(9));
	ringing->setContact(buildContact(contactExt));
	_outbox.emplace_back(invite->getSource(), std::move(ringing));

	for (auto& target : targets)
	{
		buildInviteFork(invite, caller, target, intercom);
	}
}

bool RequestsHandler::huntRingNext(const std::shared_ptr<Session>& session)
{
	// Ring the next not-yet-tried hunt member. Returns false when the list is
	// exhausted (caller fails the call). The single ringing member is kept in
	// getPendingTargets() so onOk()/onCancel() can address it like a broadcast.
	auto& members = session->getHuntMembers();
	auto invite = session->getInviteMessage();
	auto caller = session->getSrc();
	if (!invite || !caller)
	{
		return false;
	}

	while (session->getHuntIndex() < members.size())
	{
		std::string ext = members[session->getHuntIndex()];
		session->setHuntIndex(session->getHuntIndex() + 1);

		auto mc = findClient(ext);
		if (!mc.has_value())
		{
			continue;   // member went offline since the call started; skip it
		}

		session->setPendingTargets({ mc.value() });
		buildInviteFork(invite, caller, mc.value(), /*intercom=*/false);
		session->armRingTimer(std::chrono::steady_clock::now() + NO_ANSWER_TIMEOUT);
		return true;
	}

	session->clearRingTimer();
	return false;
}

bool RequestsHandler::redirectInvite(const std::shared_ptr<SipMessage>& invite,
	const std::shared_ptr<SipClient>& caller,
	const std::string& target)
{
	// Re-point an INVITE at `target` and send it as a fresh leg. Powers blind
	// transfer and the call-forward redirect paths. A new Session is allocated under
	// the SAME Call-ID so subsequent responses (180/200/BYE) route normally.
	auto targetClient = findClient(target);
	if (!targetClient.has_value())
	{
		return false;
	}

	// Don't double-allocate if a session for this Call-ID already exists (e.g. CFU
	// from onInvite, which hasn't created one yet) — reuse or create as needed.
	std::shared_ptr<Session> session;
	auto existing = getSession(invite->getCallID());
	if (existing.has_value())
	{
		session = existing.value();
		session->setDest(targetClient.value());
	}
	else
	{
		session = allocateSession(std::string(invite->getCallID()), caller);
		if (!session)
		{
			std::shared_ptr<SipMessage> responseObj = getMessageFromPool(invite->toString(), invite->getSource());
			responseObj->setHeader("SIP/2.0 503 Service Unavailable");
			responseObj->clearBody();
			responseObj->setContact(buildContact(caller->getNumber()));
			_outbox.emplace_back(invite->getSource(), std::move(responseObj));
			return true;   // we DID handle it (with a 503); target lookup succeeded
		}
		_sessions.emplace(invite->getCallID(), session);
	}

	buildInviteFork(invite, caller, targetClient.value(), /*intercom=*/false);
	return true;
}

std::shared_ptr<SipMessage> RequestsHandler::buildReferNotify(const std::shared_ptr<SipMessage>& refer,
	const std::shared_ptr<SipClient>& transferor,
	const std::string& sipfrag,
	bool terminated)
{
	// RFC 3515 §2.4.5 NOTIFY: Event: refer + message/sipfrag body reporting the
	// transfer result. Sent within the REFER's dialog back to the transferor.
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &transferor->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(transferor->getAddress().sin_port));
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	std::string branch = "z9hG4bK" + IDGen::GenerateID(12);

	std::string body = sipfrag + "\r\n";
	std::string subState = terminated ? "terminated;reason=noresource" : "active;expires=60";

	std::ostringstream ss;
	ss << "NOTIFY sip:" << transferor->getNumber() << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "From: " << refer->getTo() << "\r\n"
	   << "To: " << refer->getFrom() << "\r\n"
	   << "Call-ID: " << refer->getCallID() << "\r\n"
	   << "CSeq: 2 NOTIFY\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Event: refer\r\n"
	   << "Subscription-State: " << subState << "\r\n"
	   << "Contact: <sip:server@" << srcIpPort << ">\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Type: message/sipfrag;version=2.0\r\n"
	   << "Content-Length: " << body.size() << "\r\n\r\n"
	   << body;

	return getMessageFromPool(ss.str(), transferor->getAddress());
}

std::shared_ptr<SipMessage> RequestsHandler::buildCancel(const std::shared_ptr<SipMessage>& invite,
	const std::shared_ptr<SipClient>& target)
{
	// Build a CANCEL for an outstanding forked INVITE leg toward `target`, derived
	// from the original INVITE (same Call-ID / branch). Mirrors the inline CANCEL
	// construction used by onCancel()/onOk() for the 999 path.
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	std::string serverIpPort = activeIp + ":" + std::to_string(_serverPort);

	auto cancelMsg = getMessageFromPool(invite->toString(), invite->getSource());

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &target->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
	std::string targetIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(target->getAddress().sin_port));

	cancelMsg->setHeader("CANCEL sip:" + target->getNumber() + "@" + targetIpPort + " SIP/2.0");
	cancelMsg->setTo("To: <sip:" + target->getNumber() + "@" + serverIpPort + ">");

	std::string cseq(invite->getCSeq());
	size_t invitePos = cseq.find("INVITE");
	if (invitePos != std::string::npos)
	{
		cseq.replace(invitePos, 6, "CANCEL");
		cancelMsg->setCSeq(cseq);
	}
	cancelMsg->clearBody();
	return cancelMsg;
}

bool RequestsHandler::setCallState(std::string_view callID, Session::State state)
{
	auto session = getSession(callID);
	if (session)
	{
		session->get()->setState(state);
		return true;
	}

	return false;
}

void RequestsHandler::endCall(std::string_view callID, std::string_view srcNumber, std::string_view destNumber, std::string_view reason)
{
	// DTMF accumulators are keyed by Call-ID and share the dialog lifecycle; drop
	// this dialog's entry so _dtmfState can't grow unbounded across calls (Fix #4).
	_dtmfState.erase(std::string(callID));

	// Reclaim any park-orbit slot owned by this dialog (parked leg torn down by any
	// path: BYE, lease loss, force-disconnect). No-op when the dialog isn't parked.
	freeParkSlot(callID);

	// Capture the session (for CDR start time / final state) BEFORE we erase it.
	std::shared_ptr<Session> ending;
	auto sit = _sessions.find(std::string(callID));
	if (sit != _sessions.end())
	{
		ending = sit->second;
	}

	if (_sessions.erase(std::string(callID)) > 0)
	{
		// Record exactly once per torn-down dialog (Phase 2 CDR).
		recordCdr(ending, srcNumber, destNumber);

		std::ostringstream message;
		message << "Session has been disconnected between " << srcNumber << " and " << destNumber;
		if (!reason.empty())
		{
			message << " because " << reason;
		}
		queueLog(message.str());
	}

	for (auto& session : _sessionPool)
	{
		if (session->getCallID() == callID)
		{
			session->release();
			break;
		}
	}

	// Media beachhead safety net: if the dialog being torn down owns the live RTP
	// tone stream (BYE/CANCEL paths already call stop(); this also covers lease
	// expiry / force-disconnect / hunt cleanup that route through endCall()), stop
	// it so the socket + 20 ms task never leak past the call. Idempotent no-op when
	// the stream is idle or owned by a different Call-ID.
	_rtpSender.stop(std::string(callID));
}

uint64_t RequestsHandler::nowEpochMs() const
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

void RequestsHandler::recordCdr(const std::shared_ptr<Session>& session,
	std::string_view srcNumber, std::string_view destNumber)
{
	CallDetailRecord rec;
	rec.caller = std::string(srcNumber);
	rec.callee = std::string(destNumber);

	uint64_t startMs = nowEpochMs();
	uint32_t durationSec = 0;
	CdrResult result = CdrResult::Failed;

	if (session)
	{
		auto now = std::chrono::steady_clock::now();
		startMs = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				session->getStartTime().time_since_epoch()).count());

		switch (session->getState())
		{
			// Both Connected and Bye are "answered": a normal call ends via BYE, which
			// sets the state to Bye (NOT Connected) just before endCall() runs, while
			// the echo (777) path tears down straight from Connected. Session::setState
			// resets _startTime to the connect instant on the Connected transition and
			// the later Bye transition does NOT touch it, so getStartTime() still marks
			// the answer instant in both cases — talk time is now - startTime.
			case Session::State::Connected:
			case Session::State::Held:   // a held call is an answered call; talk time spans the hold
			case Session::State::Bye:
				result = CdrResult::Answered;
				{
					int64_t secs = static_cast<int64_t>(
						std::chrono::duration_cast<std::chrono::seconds>(
							now - session->getStartTime()).count());
					if (secs < 0) secs = 0;
					durationSec = static_cast<uint32_t>(secs);
				}
				break;
			case Session::State::Busy:        result = CdrResult::Busy;        break;
			case Session::State::Cancel:      result = CdrResult::Cancelled;   break;
			case Session::State::Unavailable: result = CdrResult::Unavailable; break;
			default:                          result = CdrResult::Failed;      break;
		}
	}

	rec.startMs = startMs;
	rec.durationSec = durationSec;
	rec.result = result;

	// Fixed ring write: overwrite the oldest slot once full (no heap growth).
	_cdrRing[_cdrHead] = std::move(rec);
	_cdrHead = (_cdrHead + 1) % POCKETDIAL_CDR_RECORDS;
	if (_cdrCount < POCKETDIAL_CDR_RECORDS)
	{
		++_cdrCount;
	}

	// Persist the ring so records survive reboot (write-through on teardown; no-op
	// on host). Caller (endCall) holds _mutex. See persistCdrRing() for the wear note.
	persistCdrRing();
}

bool RequestsHandler::registerClient(std::shared_ptr<SipClient>)
{
	return true;
}

void RequestsHandler::unregisterClient(std::string_view number)
{
	queueLog("Unregistered client: " + std::string(number));
	for (auto& client : _clientPool)
	{
		if (client->getNumber() == number)
		{
			client->release();
			break;
		}
	}
}

int RequestsHandler::parseRequestedExpires(const std::shared_ptr<SipMessage>& data) const
{
	// Helper: read a non-negative integer starting at `from`. Returns -1 if no digits.
	auto readInt = [](const std::string& s, size_t from) -> int {
		while (from < s.size() && std::isspace(static_cast<unsigned char>(s[from]))) ++from;
		size_t end = from;
		while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end]))) ++end;
		if (end == from) return -1;
		int val = 0;
		for (size_t i = from; i < end; ++i)
		{
			if (val > 200000000) return 200000000;
			val = val * 10 + (s[i] - '0');
		}
		return val;
	};

	// 1. expires= parameter on the Contact header (most common form).
	std::string contact(data->getContact());
	auto cpos = contact.find("expires=");
	if (cpos != std::string::npos)
	{
		int v = readInt(contact, cpos + 8);
		if (v >= 0) return v;
	}

	// 2. Standalone Expires header — scan only lines beginning with 'e'/'E' to
	//    avoid copying and lowercasing the entire message (which includes the SDP body).
	const std::string& raw = data->toString();
	size_t pos = 0;
	while (pos < raw.size())
	{
		size_t lineEnd = raw.find('\n', pos);
		size_t next = (lineEnd == std::string::npos) ? raw.size() : lineEnd + 1;
		char first = raw[pos];
		if (first == 'e' || first == 'E')
		{
			// Check for "expires:" (case-insensitive, 8 chars + colon)
			if (next - pos >= 9)
			{
				std::string name = raw.substr(pos, 8);
				std::transform(name.begin(), name.end(), name.begin(),
					[](unsigned char c){ return static_cast<char>(std::tolower(c)); });
				if (name == "expires:")
				{
					// Skip past any \r before \n in the offset calculation
					int v = readInt(raw, pos + 8);
					if (v >= 0) return v;
				}
			}
		}
		else if (first == '\r' || first == '\n')
		{
			break; // reached the header/body boundary blank line
		}
		pos = next;
	}

	// 3. No expiry specified — grant the default lease.
	return DEFAULT_EXPIRES;
}

void RequestsHandler::sweepExpired()
{
	auto now = std::chrono::steady_clock::now();
	for (auto& client : _clientPool)
	{
		if (client->getNumber().empty())
			continue;

		bool keepAliveTimedOut = (now - client->getLastActiveTime() > std::chrono::seconds(15));
		bool leaseExpired = client->isExpired(now);

		if (keepAliveTimedOut || leaseExpired)
		{
			if (keepAliveTimedOut)
			{
				queueLog("Pruning client due to missed OPTIONS keepalive pings: " + client->getNumber());
			}
			else
			{
				queueLog("Registration lease expired: " + client->getNumber());
			}

			// Clean up sessions involving this client
			std::string extension = client->getNumber();
			for (auto sit = _sessions.begin(); sit != _sessions.end(); )
			{
				bool involved = false;
				if (sit->second->getSrc() && sit->second->getSrc()->getNumber() == extension)
					involved = true;
				if (sit->second->getDest() && sit->second->getDest()->getNumber() == extension)
					involved = true;
				if (involved)
				{
					std::string callID = sit->first;
					// Media beachhead: if this dialog owned the live RTP tone stream,
					// stop it so a caller whose lease expires mid-stream doesn't leak
					// the socket/task. Idempotent no-op otherwise.
					_rtpSender.stop(callID);
					sit = _sessions.erase(sit);
					for (auto& session : _sessionPool)
					{
						if (session->getCallID() == callID)
						{
							session->release();
							break;
						}
					}
				}
				else
				{
					++sit;
				}
			}

			client->release();
		}
	}
}

void RequestsHandler::maybeSweep()
{
	auto now = std::chrono::steady_clock::now();
	if (now - _lastSweep < SWEEP_INTERVAL)
	{
		return;
	}
	_lastSweep = now;
	sweepExpired();
}

std::optional<std::shared_ptr<SipClient>> RequestsHandler::findClient(std::string_view number)
{
	for (auto& client : _clientPool)
	{
		if (client->getNumber() == number)
			return client;
	}
	return {};
}

void RequestsHandler::endHandle(std::string_view destNumber, std::shared_ptr<SipMessage> message)
{
	auto destClient = findClient(destNumber);
	if (destClient.has_value())
	{
		_outbox.emplace_back(destClient.value()->getAddress(), std::move(message));
	}
	else
	{
		// Clone the message so we don't mutate a shared object's header
		auto notFound = getMessageFromPool(message->toString(), message->getSource());
		notFound->setHeader(SipMessageTypes::NOT_FOUND);
		notFound->clearBody();
		auto src = message->getSource();
		_outbox.emplace_back(src, std::move(notFound));
	}
}

std::string RequestsHandler::buildContact(std::string_view number) const
{
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	return "Contact: <sip:" + std::string(number) + "@" + activeIp + ":" + std::to_string(_serverPort) + ";transport=UDP>";
}

// ── Dashboard query API ──────────────────────────────────────────────────────

static const char* sessionStateToString(Session::State s)
{
	switch (s)
	{
		case Session::State::Invited:     return "Invited";
		case Session::State::Connected:   return "Connected";
		case Session::State::Held:        return "Held";
		case Session::State::Busy:        return "Busy";
		case Session::State::Unavailable: return "Unavailable";
		case Session::State::Cancel:      return "Cancel";
		case Session::State::Bye:         return "Bye";
		default:                          return "Unknown";
	}
}

// One dashboard session-view row from a live Session. Single source of truth for
// the (caller, callee, state, talk-seconds) tuple, shared by tick()'s snapshot
// build and refreshSessionSnapshot(). Talk time is counted only once the call is
// answered (Connected or Held — a held call is still an answered call).
static std::tuple<std::string, std::string, std::string, int> sessionTuple(
	const std::shared_ptr<Session>& session,
	std::chrono::steady_clock::time_point now)
{
	std::string caller = session->getSrc() ? session->getSrc()->getNumber() : "?";
	std::string callee = session->getDest() ? session->getDest()->getNumber() : "?";
	int durationSec = 0;
	if (session->getState() == Session::State::Connected ||
		session->getState() == Session::State::Held)
	{
		durationSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
			now - session->getStartTime()).count());
	}
	return {caller, callee, sessionStateToString(session->getState()), durationSec};
}

void RequestsHandler::refreshSessionSnapshot()
{
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> snapLock(_snapshotMutex);
	_snapshot.sessions.clear();
	_snapshot.sessions.reserve(_sessions.size());
	for (const auto& [callID, session] : _sessions)
	{
		(void)callID;
		_snapshot.sessions.push_back(sessionTuple(session, now));
	}
}

std::vector<std::pair<std::string, std::string>> RequestsHandler::getActiveClients()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.clients;
}

std::vector<std::tuple<std::string, std::string, std::string, int>> RequestsHandler::getActiveSessions()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.sessions;
}

void RequestsHandler::forceDisconnect(const std::string& extension)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		queueLog("Admin: force-disconnecting extension " + extension);
		for (auto& client : _clientPool)
		{
			if (client->getNumber() == extension)
			{
				client->release();
				break;
			}
		}
		// Also remove any sessions involving this extension
		for (auto it = _sessions.begin(); it != _sessions.end(); )
		{
			bool involved = false;
			if (it->second->getSrc() && it->second->getSrc()->getNumber() == extension)
				involved = true;
			if (it->second->getDest() && it->second->getDest()->getNumber() == extension)
				involved = true;
			if (involved)
			{
				std::string callID = it->first;
				_rtpSender.stop(callID);   // media beachhead: stop any owned RTP stream
				it = _sessions.erase(it);
				for (auto& session : _sessionPool)
				{
					if (session->getCallID() == callID)
					{
						session->release();
						break;
					}
				}
			}
			else
			{
				++it;
			}
		}
		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

uint64_t RequestsHandler::getPacketsProcessed() const
{
	return _packetsProcessed.load(std::memory_order_relaxed);
}

uint64_t RequestsHandler::getPacketsDropped() const
{
	return _packetsDropped.load(std::memory_order_relaxed);
}

std::vector<CallDetailRecord> RequestsHandler::getCallDetailRecords()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.cdr;
}

void RequestsHandler::setDnd(const std::string& extension, bool on)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (on)
		{
			// Bound the map so a flood of distinct extensions can't grow the heap
			// without limit. Mirror the client-pool cap; reject new keys past it.
			if (_dnd.find(extension) == _dnd.end() &&
				_dnd.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
			{
				queueLog("DND set ignored (table full) for extension " + extension, true);
			}
			else
			{
				_dnd[extension] = true;
			}
		}
		else
		{
			// Turning DND off frees the slot, so the map only ever holds the
			// extensions that are actively in DND (bounded by registrations).
			_dnd.erase(extension);
		}
		queueLog("DND " + std::string(on ? "enabled" : "disabled") + " for extension " + extension);

		// Refresh the DND view in the dashboard snapshot immediately so the UI
		// reflects the change without waiting for the next tick().
		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			_snapshot.dnd.clear();
			_snapshot.dnd.reserve(_dnd.size());
			for (const auto& [ext, enabled] : _dnd)
			{
				if (enabled) _snapshot.dnd.push_back(ext);
			}
		}

		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

bool RequestsHandler::isDndEnabled(const std::string& extension)
{
	// Internal lookup: invoked from onInvite() which already holds _mutex, so this
	// must NOT take _mutex (std::mutex is non-recursive). Bounded map lookup.
	auto it = _dnd.find(extension);
	return it != _dnd.end() && it->second;
}

std::vector<std::string> RequestsHandler::getDndExtensions()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.dnd;
}

// ── Call forwarding (CFU/CFB/CFNA) ───────────────────────────────────────────

std::string RequestsHandler::getForwardTarget(const std::string& extension, const std::string& trigger) const
{
	// Internal lookup invoked from onInvite()/onBusy()/tick(), all of which already
	// hold _mutex — must NOT lock (non-recursive). Bounded map lookup.
	auto it = _forwards.find(extension);
	if (it == _forwards.end())
	{
		return {};
	}
	if (trigger == "always")   return it->second.always;
	if (trigger == "busy")     return it->second.busy;
	if (trigger == "noanswer") return it->second.noAnswer;
	return {};
}

void RequestsHandler::setForward(const std::string& extension, const std::string& trigger, const std::string& target)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);

		// Reject the virtual extensions outright; they are not real endpoints.
		// 98x is the reserved paging-zone range — also not a forwardable endpoint.
		if (extension == "777" || extension == "999" || pbx::isPageZoneExt(extension))
		{
			queueLog("Forward set ignored for virtual extension " + extension, true);
		}
		else
		{
			auto it = _forwards.find(extension);
			bool isNew = (it == _forwards.end());

			// Bound the table like _dnd: refuse a brand-new extension past the cap.
			if (isNew && _forwards.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
			{
				queueLog("Forward set ignored (table full) for extension " + extension, true);
			}
			else
			{
				pbx::ForwardConfig& cfg = _forwards[extension];
				if (trigger == "always")        cfg.always   = target;
				else if (trigger == "busy")     cfg.busy     = target;
				else if (trigger == "noanswer") cfg.noAnswer = target;

				// Drop the entry entirely once no trigger remains set, so the map only
				// holds actively-forwarded extensions (bounded by registrations).
				if (cfg.empty())
				{
					_forwards.erase(extension);
				}
				queueLog("Forward " + trigger + " for " + extension +
					(target.empty() ? " cleared" : (" -> " + target)));
				persistForwards();
			}
		}

		// Refresh the dashboard snapshot immediately (mirror setDnd).
		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			_snapshot.forwards.clear();
			_snapshot.forwards.reserve(_forwards.size());
			for (const auto& [ext, cfg] : _forwards)
			{
				_snapshot.forwards.emplace_back(ext, cfg.always, cfg.busy, cfg.noAnswer);
			}
		}

		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

std::vector<std::tuple<std::string, std::string, std::string, std::string>> RequestsHandler::getForwards()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.forwards;
}

// ── Ring / hunt groups ───────────────────────────────────────────────────────

const pbx::RingGroup* RequestsHandler::findRingGroup(const std::string& extension) const
{
	// Internal lookup from onInvite() (already holds _mutex). Bounded map lookup.
	auto it = _ringGroups.find(extension);
	return (it == _ringGroups.end()) ? nullptr : &it->second;
}

void RequestsHandler::setRingGroup(const std::string& groupExt, const std::string& members, const std::string& mode)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);

		if (groupExt == "777" || groupExt == "999" || pbx::isPageZoneExt(groupExt))
		{
			queueLog("Ring group ignored for reserved extension " + groupExt, true);
		}
		else
		{
			std::vector<std::string> list = pbx::splitMembers(members);
			if (list.empty())
			{
				// Empty membership deletes the group.
				_ringGroups.erase(groupExt);
				queueLog("Ring group " + groupExt + " deleted");
				persistRingGroups();
			}
			else
			{
				bool isNew = (_ringGroups.find(groupExt) == _ringGroups.end());
				if (isNew && _ringGroups.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
				{
					queueLog("Ring group ignored (table full) for " + groupExt, true);
				}
				else
				{
					pbx::RingGroup& g = _ringGroups[groupExt];
					g.members = std::move(list);
					g.mode = (mode == "hunt") ? pbx::GroupMode::Hunt : pbx::GroupMode::RingAll;
					queueLog("Ring group " + groupExt + " (" +
						(g.mode == pbx::GroupMode::Hunt ? "hunt" : "ringall") + ") = " +
						pbx::joinMembers(g.members));
					persistRingGroups();
				}
			}
		}

		// Refresh dashboard snapshot immediately.
		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			_snapshot.ringGroups.clear();
			_snapshot.ringGroups.reserve(_ringGroups.size());
			for (const auto& [ext, g] : _ringGroups)
			{
				_snapshot.ringGroups.emplace_back(ext,
					g.mode == pbx::GroupMode::Hunt ? "hunt" : "ringall",
					pbx::joinMembers(g.members));
			}
		}

		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

std::vector<std::tuple<std::string, std::string, std::string>> RequestsHandler::getRingGroups()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.ringGroups;
}

// ── Paging zones (980–989) ────────────────────────────────────────────────────

const pbx::PageZone* RequestsHandler::findPageZone(const std::string& extension) const
{
	// Internal lookup from onInvite() (already holds _mutex). Bounded map lookup.
	auto it = _pageZones.find(extension);
	return (it == _pageZones.end()) ? nullptr : &it->second;
}

bool RequestsHandler::isPageZoneDialog(const std::string& extension) const
{
	// Caller holds _mutex. True only for a CONFIGURED zone, so an unconfigured 98x
	// dialog (which routed through normal handling) keeps normal teardown.
	return pbx::isPageZoneExt(extension) && _pageZones.find(extension) != _pageZones.end();
}

void RequestsHandler::setPageZone(const std::string& zoneExt, const std::string& members)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);

		if (!pbx::isPageZoneExt(zoneExt))
		{
			queueLog("Page zone ignored for non-zone extension " + zoneExt +
				" (zones are 980-989)", true);
		}
		else
		{
			std::vector<std::string> list = pbx::splitZoneMembers(members);
			if (list.empty())
			{
				// Empty membership deletes the zone.
				_pageZones.erase(zoneExt);
				queueLog("Page zone " + zoneExt + " deleted");
				persistPageZones();
			}
			else
			{
				bool isNew = (_pageZones.find(zoneExt) == _pageZones.end());
				if (isNew && _pageZones.size() >= static_cast<size_t>(POCKETDIAL_MAX_PAGE_ZONES))
				{
					queueLog("Page zone ignored (table full) for " + zoneExt, true);
				}
				else
				{
					pbx::PageZone& z = _pageZones[zoneExt];
					z.members = std::move(list);
					queueLog("Page zone " + zoneExt + " = " + pbx::joinMembers(z.members));
					persistPageZones();
				}
			}
		}

		// Refresh dashboard snapshot immediately (mirror setRingGroup).
		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			_snapshot.pageZones.clear();
			_snapshot.pageZones.reserve(_pageZones.size());
			for (const auto& [ext, z] : _pageZones)
			{
				_snapshot.pageZones.emplace_back(ext, pbx::joinMembers(z.members));
			}
		}

		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

std::vector<std::pair<std::string, std::string>> RequestsHandler::getPageZones()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.pageZones;
}

// ── Registrar mode (STAGE 2) ──────────────────────────────────────────────────

void RequestsHandler::setRegistrarMode(RegistrarMode mode)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_registrarMode.store(mode, std::memory_order_relaxed);
		persistRegistrarMode();
		const char* name = (mode == RegistrarMode::Open)   ? "open"
		                 : (mode == RegistrarMode::Learn)  ? "learn"
		                                                   : "secure";
		queueLog(std::string("Registrar mode set to ") + name);
		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}
	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

RequestsHandler::RegistrarMode RequestsHandler::getRegistrarMode() const
{
	// Lock-free read: the dashboard polls this; onRegister() branches on it on the
	// hot path. The atomic guarantees a torn-free load.
	return _registrarMode.load(std::memory_order_relaxed);
}

void RequestsHandler::setRewarmMinutes(uint16_t minutes)
{
	if (minutes > 1440) minutes = 1440;   // cap at 24 h; 0 = disabled
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_rewarmMinutes.store(minutes, std::memory_order_relaxed);
		persistRewarmInterval();
		// Apply live: push the new cadence (seconds) into the running anchor (no-op on
		// loopback). setRewarmIntervalSec() is just an atomic store — safe under _mutex.
		if (_anchorClient)
		{
			_anchorClient->setRewarmIntervalSec(static_cast<uint32_t>(minutes) * 60u);
		}
		queueLog(std::string("Anchor TLS re-warm cadence set to ") + std::to_string(minutes) +
		         (minutes ? " min" : " min (disabled)"));
		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}
	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
}

uint16_t RequestsHandler::getRewarmMinutes() const
{
	// Lock-free read for the dashboard/TUI snapshot.
	return _rewarmMinutes.load(std::memory_order_relaxed);
}

// ── Device registry (STAGE 2: Learn-mode adoption) ────────────────────────────

void RequestsHandler::refreshDeviceSnapshot()
{
	// Caller holds _mutex. Mirror _devices into the dashboard snapshot, preserving
	// the previously-published online flags (online is tracked in the snapshot, not
	// in _devices, since it is volatile registration state — not persisted).
	std::lock_guard<std::mutex> snapLock(_snapshotMutex);

	// Index the existing online flags by MAC so a rebuild doesn't lose them.
	std::unordered_map<std::string, bool> wasOnline;
	wasOnline.reserve(_snapshot.devices.size());
	for (const auto& d : _snapshot.devices)
	{
		wasOnline[d.mac] = d.online;
	}

	_snapshot.devices.clear();
	_snapshot.devices.reserve(_devices.size());
	for (const auto& [mac, rec] : _devices)
	{
		AdoptedDevice d;
		d.mac = mac;
		d.extension = rec.extension;
		d.state = rec.state;
		auto it = wasOnline.find(mac);
		d.online = (it != wasOnline.end()) ? it->second : false;
		_snapshot.devices.push_back(std::move(d));
	}
}

void RequestsHandler::markDeviceOnline(const std::string& mac, bool online)
{
	// Caller holds _mutex. Online state lives only in the snapshot (volatile, not
	// persisted). No-op if the device isn't adopted (e.g. Open mode never records).
	std::lock_guard<std::mutex> snapLock(_snapshotMutex);
	for (auto& d : _snapshot.devices)
	{
		if (d.mac == mac)
		{
			d.online = online;
			return;
		}
	}
}

std::vector<RequestsHandler::AdoptedDevice> RequestsHandler::getAdoptedDevices()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.devices;
}

bool RequestsHandler::secureDevice(const std::string& macOrExt)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(_mutex);

		// Accept either a 12-hex MAC (direct key) or an extension (find the device
		// currently adopted under it).
		auto it = _devices.find(macOrExt);
		if (it == _devices.end())
		{
			for (auto cand = _devices.begin(); cand != _devices.end(); ++cand)
			{
				if (cand->second.extension == macOrExt) { it = cand; break; }
			}
		}

		if (it != _devices.end())
		{
			// Footgun guard: promoting a device to Secured makes admitSecure() demand a
			// digest for it. If the extension has NO stored secret, that would lock the
			// phone out on its next REGISTER ("Extension Not Provisioned"). Refuse, and
			// tell the operator to assign a secret first.
			if (!SipSecretStore::hasSecret(it->second.extension))
			{
				queueLog("secureDevice: ext " + it->second.extension +
					" has no SIP secret — assign one before securing", true);
			}
			else
			{
				if (it->second.state != DeviceState::Secured)
				{
					it->second.state = DeviceState::Secured;
					persistDevices();
					changed = true;
				}
				queueLog("Device " + it->first + " (ext " + it->second.extension + ") secured");
			}
		}
		else
		{
			queueLog("secureDevice: no adopted device for '" + macOrExt + "'", true);
		}

		if (changed) refreshDeviceSnapshot();
		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}
	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
	return changed;
}

bool RequestsHandler::forgetDevice(const std::string& macOrExt)
{
	std::vector<std::pair<bool, std::string>> localLogs;
	bool removed = false;
	{
		std::lock_guard<std::mutex> lock(_mutex);

		auto it = _devices.find(macOrExt);
		if (it == _devices.end())
		{
			for (auto cand = _devices.begin(); cand != _devices.end(); ++cand)
			{
				if (cand->second.extension == macOrExt) { it = cand; break; }
			}
		}

		if (it != _devices.end())
		{
			queueLog("Device " + it->first + " (ext " + it->second.extension + ") forgotten");
			_devices.erase(it);
			persistDevices();
			removed = true;
			refreshDeviceSnapshot();
		}
		else
		{
			queueLog("forgetDevice: no adopted device for '" + macOrExt + "'", true);
		}

		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}
	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}
	return removed;
}

// ── REGISTER admission helpers (STAGE 2) ──────────────────────────────────────
// All run under _mutex (called from onRegister, which holds it via handle()).

void RequestsHandler::sendChallenge(const std::shared_ptr<SipMessage>& data, bool stale)
{
	auto response = getMessageFromPool(data->toString(), data->getSource());
	response->setHeader("SIP/2.0 401 Unauthorized");
	response->clearBody();
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	response->setTo(std::string(data->getTo()) + ";tag=" + IDGen::GenerateID(9));
	// Fresh stateless nonce per challenge; realm MUST match SipSecretStore::kRealm.
	response->addHeader("WWW-Authenticate",
		SipDigest::buildWwwAuthenticate(SipSecretStore::kRealm,
			SipDigest::generateNonce(), stale));
	response->syncContentLength();
	_outbox.emplace_back(data->getSource(), std::move(response));
}

void RequestsHandler::sendForbidden(const std::shared_ptr<SipMessage>& data, const std::string& reason)
{
	auto response = getMessageFromPool(data->toString(), data->getSource());
	response->setHeader("SIP/2.0 403 " + reason);
	response->clearBody();
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	response->syncContentLength();
	_outbox.emplace_back(data->getSource(), std::move(response));
}

RequestsHandler::AuthDecision RequestsHandler::admitSecure(
	const std::shared_ptr<SipMessage>& data, const std::string& ext, std::string& outRejectReason)
{
	// Secure mode: a provisioned extension MUST present a valid digest. An ext with
	// NO stored secret is unprovisioned — reject with a clear reason (the SAFE
	// default; "allow first-time" would defeat the point of secure mode).
	auto ha1 = SipSecretStore::getHa1(ext);
	if (!ha1.has_value())
	{
		outRejectReason = "Extension Not Provisioned";
		queueLog("Secure REGISTER for unprovisioned ext " + ext + " rejected", true);
		return AuthDecision::Reject;
	}

	SipDigest::DigestAuth auth;
	std::string_view authHdr = data->getAuthorization();
	if (authHdr.empty() || !SipDigest::parseAuthorization(std::string(authHdr), auth))
	{
		// No (parseable) credentials → challenge with a fresh nonce.
		sendChallenge(data, /*stale=*/false);
		return AuthDecision::Challenge;
	}

	// Validate the nonce we issued. A forged/garbage nonce is a hard re-challenge
	// (not stale); an expired-but-ours nonce → challenge with stale=true so the
	// phone silently retries.
	bool expired = false;
	if (!SipDigest::validateNonce(auth.nonce, &expired))
	{
		sendChallenge(data, /*stale=*/expired);
		return AuthDecision::Challenge;
	}

	// Recompute + constant-time compare. Method is REGISTER.
	if (!SipDigest::verify(auth, *ha1, std::string(data->getType())))
	{
		outRejectReason = "Bad Credentials";
		queueLog("Secure REGISTER for ext " + ext + " failed digest verify", true);
		return AuthDecision::Reject;
	}

	return AuthDecision::Accept;
}

RequestsHandler::AuthDecision RequestsHandler::admitLearn(
	const std::shared_ptr<SipMessage>& data, const std::string& ext, std::string& outRejectReason)
{
	// Learn mode = TOFU + MAC-lock.
	//   UNKNOWN mac            -> accept WITHOUT verifying, record {mac, ext, Learned}.
	//   KNOWN + Secured mac    -> enforce digest (same path as secure mode).
	//   ext Secured to a DIFFERENT mac -> reject (anti-spoof lock).
	//   first-packet ARP miss  -> accept + defer the lock to the next REGISTER.
	auto macOpt = ArpLookup::pdLookupMac(data->getSource());
	if (!macOpt.has_value())
	{
		// Cache miss (or host). Accept now; the server's 200 OK + beep + OPTIONS
		// populates the ARP cache so the NEXT REGISTER resolves and locks. Do NOT
		// hard-fail — that would brick the very first registration.
		queueLog("Learn REGISTER ext " + ext + ": ARP miss, deferring MAC-lock");
		return AuthDecision::Accept;
	}
	const std::string mac = ArpLookup::toHex12(*macOpt);

	// Anti-spoof: if this extension is already Secured to a DIFFERENT mac, reject.
	for (const auto& [m, rec] : _devices)
	{
		if (rec.extension == ext && rec.state == DeviceState::Secured && m != mac)
		{
			outRejectReason = "Extension Locked To Another Device";
			queueLog("Learn REGISTER ext " + ext + " from " + mac +
				" rejected: locked to " + m, true);
			return AuthDecision::Reject;
		}
	}

	auto it = _devices.find(mac);
	if (it == _devices.end())
	{
		// First time we've seen this MAC: trust-on-first-use. Bound the table like
		// _dnd/_forwards — a flood of distinct MACs can't grow the heap unbounded.
		if (_devices.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
		{
			outRejectReason = "Device Table Full";
			queueLog("Learn REGISTER: device table full, rejecting " + mac, true);
			return AuthDecision::Reject;
		}
		DeviceRecord rec;
		rec.extension = ext;
		rec.state = DeviceState::Learned;
		_devices.emplace(mac, std::move(rec));
		persistDevices();
		refreshDeviceSnapshot();
		queueLog("Learn: adopted device " + mac + " as ext " + ext);
		return AuthDecision::Accept;
	}

	// Known MAC. Keep its extension in sync if the phone re-provisioned to a new AOR.
	if (it->second.extension != ext)
	{
		it->second.extension = ext;
		persistDevices();
		refreshDeviceSnapshot();
	}

	if (it->second.state == DeviceState::Secured)
	{
		// Promoted device: enforce digest exactly as secure mode does.
		return admitSecure(data, ext, outRejectReason);
	}

	// Known + still Learned → accept (TOFU continues until an admin secures it).
	return AuthDecision::Accept;
}

// ── Outbound SIP MESSAGE (STAGE 2) ────────────────────────────────────────────

bool RequestsHandler::sendMessageTo(const std::string& ext, const std::string& text)
{
	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> localOutbox;
	bool sent = false;
	{
		std::lock_guard<std::mutex> lock(_mutex);

		auto client = findClient(ext);
		if (!client.has_value())
		{
			// Not registered → nothing to send to. Best-effort, no enqueue.
			return false;
		}

		const sockaddr_in& addr = client.value()->getAddress();
		char ipBuf[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
		std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));

		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);

		std::string callId  = IDGen::GenerateID(16) + "@" + activeIp;
		std::string branch  = "z9hG4bK" + IDGen::GenerateID(12);
		std::string fromTag = IDGen::GenerateID(9);

		// Bound the body so the whole datagram stays well under a typical MTU; a
		// notify is short by design.
		std::string body = text.size() > 512 ? text.substr(0, 512) : text;

		std::ostringstream ss;
		ss << "MESSAGE sip:" << ext << "@" << destIpPort << " SIP/2.0\r\n"
		   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
		   << "From: \"PocketDial\" <sip:pbx@" << srcIpPort << ">;tag=" << fromTag << "\r\n"
		   << "To: <sip:" << ext << "@" << activeIp << ">\r\n"
		   << "Call-ID: " << callId << "\r\n"
		   << "CSeq: 1 MESSAGE\r\n"
		   << "Max-Forwards: 70\r\n"
		   << "User-Agent: pocket-dial\r\n"
		   << "Content-Type: text/plain\r\n"
		   << "Content-Length: " << body.size() << "\r\n\r\n"
		   << body;

		auto msg = getMessageFromPool(ss.str(), addr);
		msg->syncContentLength();
		_outbox.emplace_back(addr, std::move(msg));
		sent = true;

		// Drain into a local vector and dispatch outside the lock (no IO under lock).
		localOutbox = std::move(_outbox);
		_outbox.clear();
	}

	for (auto& [addr, msg] : localOutbox)
	{
		_onHandled(addr, std::move(msg));
	}
	return sent;
}

size_t RequestsHandler::getClientCount()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.clients.size();
}

size_t RequestsHandler::getSessionCount()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.sessions.size();
}

RequestsHandler::Telemetry RequestsHandler::getTelemetry()
{
	Telemetry t;
	// Anchor + media are atomic reads (no registrar lock): isConnected() reads an atomic in
	// the anchor; MediaBridge.isActive() and the PlayoutBuffer counters are atomics too.
	t.anchorConnected  = (_anchorClient != nullptr && _anchorClient->isConnected());
	t.mediaActive      = _mediaBridge.isActive();
	t.playoutUnderruns = _mediaBridge.getPlayoutBuffer().getUnderruns();
	t.playoutOverruns  = _mediaBridge.getPlayoutBuffer().getOverruns();
	// Pool usage from the existing thread-safe snapshot getters (_snapshotMutex, not _mutex).
	t.clientsUsed      = getClientCount();
	t.clientsCap       = POCKETDIAL_MAX_CLIENTS;
	t.sessionsUsed     = getSessionCount();
	t.sessionsCap      = POCKETDIAL_MAX_SESSIONS;
	// Estimated anchor TLS socket draw: 3 persistent (control WS + GET + POST audio) while the
	// link is up, plus 2 per active bridged call (the per-call media GET/POST streams).
	t.tlsSocketsEst    = t.anchorConnected ? (3 + (t.mediaActive ? 2 : 0)) : 0;
	// TLS handshake split (full ECDHE vs resumed) on the POST media stream — shows resumption
	// holding and reveals 3CX's session-ticket lifetime over idle gaps.
	if (_anchorClient) _anchorClient->getTlsHandshakeStats(t.tlsFullHandshakes, t.tlsResumedHandshakes);
	return t;
}

void RequestsHandler::tick()
{
	auto now = std::chrono::steady_clock::now();
	if (now - _lastTick < std::chrono::seconds(1))
	{
		return;
	}
	_lastTick = now;

	// Periodic non-blocking pump for the WAN anchor (e.g. the 3CX _outboundActive reconcile
	// watchdog). Called outside the registrar _mutex (invariant #2); it only reads atomics and
	// may spawn its own worker for any blocking I/O.
	if (_anchorClient) _anchorClient->tick();

	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> localOutbox;
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_outbox.clear();

		sweepExpired();

		// Belt-and-suspenders (Fix #4): drop DTMF accumulators whose dialog is gone,
		// in case a teardown path bypassed endCall(). Bounded by the small session pool.
		for (auto dit = _dtmfState.begin(); dit != _dtmfState.end(); )
		{
			if (_sessions.find(dit->first) == _sessions.end()) dit = _dtmfState.erase(dit);
			else ++dit;
		}

		// No-answer timers (CFNA + hunt-group progression). Poll the armed sessions
		// and act on any that have run past their ring deadline without connecting.
		// Collected first so we don't mutate _sessions while iterating it.
		std::vector<std::string> expiredCallIds;
		for (const auto& [callID, session] : _sessions)
		{
			if (session->isRingExpired(now) && session->getState() == Session::State::Invited)
			{
				expiredCallIds.push_back(callID);
			}
		}
		for (const auto& callID : expiredCallIds)
		{
			auto sit = _sessions.find(callID);
			if (sit == _sessions.end()) continue;
			auto session = sit->second;
			session->clearRingTimer();

			if (session->isAnchorInbound())
			{
				// Inbound PSTN call no extension picked up: CANCEL every still-ringing forked
				// INVITE (server is UAC), drop the upstream leg, and end the session. No leg
				// answered (state is Invited), so pendingTargets still holds them all.
				for (const auto& target : session->getPendingTargets())
				{
					auto cancel = buildInboundCancelTo(session, target);
					if (cancel) _outbox.emplace_back(target->getAddress(), std::move(cancel));
				}
				asyncDropCall(session->getAnchorParticipantId());
				queueLog("[3CX] Inbound: no answer from " +
				         std::to_string(session->getPendingTargets().size()) + " extension(s) — cancelled");
				endCall(callID, session->getAnchorParticipantId(), "", "inbound no answer");
				continue;
			}

			if (session->isHunt())
			{
				// Advance to the next hunt member; CANCEL the member that timed out.
				for (const auto& t : session->getPendingTargets())
				{
					auto invite = session->getInviteMessage();
					if (invite)
					{
						auto cancel = buildCancel(invite, t);
						if (cancel) _outbox.emplace_back(t->getAddress(), std::move(cancel));
					}
				}
				if (!huntRingNext(session))
				{
					// List exhausted: 480 to the caller and tear down.
					auto invite = session->getInviteMessage();
					if (invite && session->getSrc())
					{
						auto resp = getMessageFromPool(invite->toString(), invite->getSource());
						resp->setHeader(SipMessageTypes::UNAVAILABLE);
						resp->clearBody();
						resp->setContact(buildContact(session->getGroupExt()));
						_outbox.emplace_back(invite->getSource(), std::move(resp));
						endCall(callID, session->getSrc()->getNumber(), session->getGroupExt(), "hunt group no answer");
					}
				}
			}
			else
			{
				// CFNA: CANCEL the original callee leg and INVITE the no-answer target.
				auto invite = session->getInviteMessage();
				auto dest = session->getDest();
				auto src = session->getSrc();
				std::string cfna = session->getNoAnswerTarget();
				if (invite && src && !cfna.empty())
				{
					if (dest)
					{
						auto cancel = buildCancel(invite, dest);
						if (cancel) _outbox.emplace_back(dest->getAddress(), std::move(cancel));
					}
					queueLog("CFNA: no answer, forwarding -> " + cfna);
					endCall(callID, src->getNumber(), std::string(invite->getToNumber()), "no answer (CFNA)");
					redirectInvite(invite, src, cfna);
				}
			}
		}

		// Sweep rate-limit buckets older than 60 seconds (Issue #58)
		for (auto rit = _rateBuckets.begin(); rit != _rateBuckets.end(); )
		{
			if (now - rit->second.last > std::chrono::seconds(60))
			{
				rit = _rateBuckets.erase(rit);
			}
			else
			{
				++rit;
			}
		}

		for (auto& client : _clientPool)
		{
			if (client->getNumber().empty()) continue;
			if (now - client->getLastPingTime() >= std::chrono::seconds(5))
			{
				client->setLastPingTime(now);
				auto ping = buildOptionsPing(client);
				_outbox.emplace_back(client->getAddress(), std::move(ping));
			}
		}

		// Register-beep timeouts: any beep dialog whose deadline has passed without the
		// phone answering (AwaitingInviteOk) is CANCELled and freed — no retransmit
		// storm, no leak. A dialog still awaiting the 200-to-BYE just frees its slot
		// (the BYE was already sent best-effort). Done here, under _mutex, enqueuing to
		// _outbox; the actual sendto() happens after the lock is dropped.
		for (std::size_t i = 0; i < _beepDialogs.size(); ++i)
		{
			auto& bd = _beepDialogs[i];
			if (bd.state == BeepState::Free || now < bd.deadline)
			{
				continue;
			}
			if (bd.state == BeepState::AwaitingInviteOk)
			{
				// No auto-answer in time: CANCEL the INVITE, but do NOT free the slot
				// yet. RFC 3261 §17.1.1.3 — the INVITE client transaction is not complete
				// until the phone's 487 final response arrives and is ACKed. Freeing here
				// (the #90 bug) orphaned that 487: findBeepByCallID() then failed, no ACK
				// went out, the phone retransmitted 487, and a CANCEL racing the phone's
				// own finalisation drew a 481. Linger in AwaitingCancelDone until
				// onReqTerminated() ACKs the 487; the re-armed deadline below guarantees a
				// silent phone still frees the slot (bounded — never leaks).
				auto cancel = buildBeepCancel(i);
				if (cancel) _outbox.emplace_back(bd.addr, std::move(cancel));
				queueLog("Register beep: no answer from " + bd.ext + ", cancelled");
				bd.state    = BeepState::AwaitingCancelDone;
				bd.deadline = now + std::chrono::seconds(5);
				continue;
			}
			// AwaitingByeOk, or AwaitingCancelDone whose linger elapsed without a 487:
			// the teardown is complete (or the phone went silent) — now safe to free.
			bd = BeepDialog{};
		}

		// Call-park orbit timeouts (ring back the parker / tear down stale parks).
		parkSweep(now);

		// BLF: expire overdue subscriptions (terminal NOTIFY, reason=timeout), then
		// run the change-detection pass so timer-driven transitions (sweeps, CFNA,
		// hunt advance) light/clear lamps too. NOTIFYs go into _outbox under _mutex
		// and are sent after release, like everything else in this pass.
		sweepSubscriptions();
		refreshSubscriptions();

		// Build snapshot under registrar mutex lock, then save it under snapshot mutex lock
		RegistrarSnapshot nextSnapshot;
		nextSnapshot.packetsProcessed = _packetsProcessed.load(std::memory_order_relaxed);
		nextSnapshot.packetsDropped = _packetsDropped.load(std::memory_order_relaxed);
		for (const auto& client : _clientPool)
		{
			if (client->getNumber().empty()) continue;
			const auto& addr = client->getAddress();
			char ipBuf[INET_ADDRSTRLEN]{};
			inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
			std::string ipPort = std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));
			nextSnapshot.clients.emplace_back(client->getNumber(), ipPort);
		}

		nextSnapshot.sessions.reserve(_sessions.size());
		for (const auto& [callID, session] : _sessions)
		{
			(void)callID;
			nextSnapshot.sessions.push_back(sessionTuple(session, now));
		}

		// Parked calls (orbit table) into the dashboard view. Mirrors refreshParkSnapshot
		// — done here too so the 1 Hz wholesale snapshot swap keeps the parked rows.
		for (const auto& slot : _parkSlots)
		{
			if (slot.state == ParkState::Free) continue;
			int secs = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
				now - slot.parkedAt).count());
			nextSnapshot.parked.emplace_back(slot.orbit, slot.parkedExt, slot.parker, secs);
		}

		// CDR view: copy the ring out newest-first into the snapshot.
		nextSnapshot.cdr.reserve(_cdrCount);
		for (size_t i = 0; i < _cdrCount; ++i)
		{
			// _cdrHead points one past the newest; walk backwards with wrap.
			size_t idx = (_cdrHead + POCKETDIAL_CDR_RECORDS - 1 - i) % POCKETDIAL_CDR_RECORDS;
			nextSnapshot.cdr.push_back(_cdrRing[idx]);
		}

		// DND view: extensions currently in DND.
		nextSnapshot.dnd.reserve(_dnd.size());
		for (const auto& [ext, enabled] : _dnd)
		{
			if (enabled) nextSnapshot.dnd.push_back(ext);
		}

		// Call-forward view.
		nextSnapshot.forwards.reserve(_forwards.size());
		for (const auto& [ext, cfg] : _forwards)
		{
			nextSnapshot.forwards.emplace_back(ext, cfg.always, cfg.busy, cfg.noAnswer);
		}

		// Ring/hunt-group view.
		nextSnapshot.ringGroups.reserve(_ringGroups.size());
		for (const auto& [ext, g] : _ringGroups)
		{
			nextSnapshot.ringGroups.emplace_back(ext,
				g.mode == pbx::GroupMode::Hunt ? "hunt" : "ringall",
				pbx::joinMembers(g.members));
		}

		// Paging-zone view.
		nextSnapshot.pageZones.reserve(_pageZones.size());
		for (const auto& [ext, z] : _pageZones)
		{
			nextSnapshot.pageZones.emplace_back(ext, pbx::joinMembers(z.members));
		}

		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			_snapshot = std::move(nextSnapshot);
		}

		localOutbox = std::move(_outbox);
		_outbox.clear();
		// Drain out-of-band sends (WS Answered/Dropped callbacks) on this tick too,
		// so a 200 OK still goes out even if no inbound SIP packet arrives to trigger
		// handle(). Never cleared at the start of tick(), so it can't be lost.
		for (auto& e : _asyncOutbox) localOutbox.push_back(std::move(e));
		_asyncOutbox.clear();

		localLogs = std::move(_logQueue);
		_logQueue.clear();
	}

	for (const auto& log : localLogs)
	{
		if (log.first) std::cerr << log.second << std::endl;
		else std::cout << log.second << std::endl;
	}

	for (auto& event : localOutbox)
	{
		_onHandled(event.first, std::move(event.second));
	}

	// Issue #42/#55: debounced NVS write-back. The persistXxx() mutators ran under
	// _mutex during this tick (or an intervening handle()) and only set dirty bits;
	// the actual flash commit happens here, OUTSIDE the registrar lock, so a 10–100 ms
	// erase/program never stalls SIP signaling. No-op on host.
	flushDirtyNvs();

#if !defined(ESP_PLATFORM) && !defined(ESP32)
	// Issue #67: reap finished host anchor-call workers so they don't accumulate as
	// joinable thread objects until destruction. (ESP uses self-deleting tasks.)
	reapAnchorWorkers();
#endif
}

std::optional<std::shared_ptr<SipClient>> RequestsHandler::findClientByAddress(const sockaddr_in& addr)
{
	for (auto& client : _clientPool)
	{
		if (client->getNumber().empty()) continue;
		if (client->getAddress().sin_addr.s_addr == addr.sin_addr.s_addr &&
			client->getAddress().sin_port == addr.sin_port)
		{
			return client;
		}
	}
	return {};
}

std::shared_ptr<SipMessage> RequestsHandler::buildOptionsPing(const std::shared_ptr<SipClient>& client)
{
	std::string clientNum = client->getNumber();

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &client->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(client->getAddress().sin_port));

	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);

	std::string callId = IDGen::GenerateID(16) + "@" + activeIp;
	std::string branch = "z9hG4bK" + IDGen::GenerateID(12);
	std::string fromTag = IDGen::GenerateID(9);

	std::ostringstream ss;
	ss << "OPTIONS sip:" << clientNum << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "To: <sip:" << clientNum << "@" << destIpPort << ">\r\n"
	   << "From: <sip:server@" << srcIpPort << ">;tag=" << fromTag << "\r\n"
	   << "Call-ID: " << callId << "\r\n"
	   << "CSeq: 1 OPTIONS\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Length: 0\r\n\r\n";

	return getMessageFromPool(ss.str(), client->getAddress());
}

// ── Register beep (signaling-only intercom tone) ─────────────────────────────
//
// On a new REGISTER, the registrar acts as a UAC: it sends the phone a brief
// auto-answer INVITE (same header set the 999 all-page uses) so the handset plays
// its intercom alert tone, then immediately tears the call back down (ACK → BYE on
// the phone's 200, or CANCEL on timeout). NO RTP is sourced — the SDP offers a
// single payload at a=inactive purely so the offer is well-formed. Each dialog is a
// small fixed record in _beepDialogs, bounded by POCKETDIAL_MAX_BEEPS; if the table
// is full the beep is simply skipped (it is cosmetic). All helpers below assume the
// caller already holds _mutex (non-recursive) and only enqueue to _outbox.

RequestsHandler::BeepDialog* RequestsHandler::findBeepByCallID(std::string_view callID)
{
	// SipMessage::getCallID() yields the FULL header line ("Call-ID: <value>"), but
	// sendRegisterBeep() stores slot->callID as the BARE value (it builds "Call-ID: " +
	// callID itself). Normalise the incoming key to its bare value before comparing, or
	// the lookup never matches — which silently broke the answered-beep ACK/BYE path and
	// blocked the #90 teardown ACK. Caller holds _mutex.
	if (size_t colon = callID.find(':'); colon != std::string_view::npos)
	{
		callID.remove_prefix(colon + 1);
		while (!callID.empty() && (callID.front() == ' ' || callID.front() == '\t'))
		{
			callID.remove_prefix(1);
		}
	}
	for (auto& bd : _beepDialogs)
	{
		if (bd.state != BeepState::Free && std::string_view(bd.callID) == callID)
		{
			return &bd;
		}
	}
	return nullptr;
}

void RequestsHandler::sendRegisterBeep(const std::shared_ptr<SipClient>& phone)
{
	if (!phone || phone->getNumber().empty())
	{
		return;
	}

	// Grab a free beep slot. Full table → skip (cosmetic); no leak, no blocking.
	BeepDialog* slot = nullptr;
	for (auto& bd : _beepDialogs)
	{
		if (bd.state == BeepState::Free) { slot = &bd; break; }
	}
	if (!slot)
	{
		queueLog("Register beep: table full, skipping beep for " + phone->getNumber());
		return;
	}

	std::string clientNum = phone->getNumber();
	const sockaddr_in& addr = phone->getAddress();

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));

	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);

	std::string callId  = IDGen::GenerateID(16) + "@" + activeIp;
	std::string branch  = "z9hG4bK" + IDGen::GenerateID(12);
	std::string fromTag = IDGen::GenerateID(9);

	// Record the dialog BEFORE sending so a (synchronous) response can never race
	// ahead of the bookkeeping.
	slot->state    = BeepState::AwaitingInviteOk;
	slot->callID   = callId;
	slot->branch   = branch;
	slot->fromTag  = fromTag;
	slot->ext      = clientNum;
	slot->addr     = addr;
	slot->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

	// Minimal, well-formed SDP. a=inactive: no RTP will flow (server sources none).
	std::string body =
		"v=0\r\n"
		"o=- 0 0 IN IP4 " + activeIp + "\r\n"
		"s=pocket-dial\r\n"
		"c=IN IP4 " + activeIp + "\r\n"
		"t=0 0\r\n"
		"m=audio 9 RTP/AVP 0\r\n"
		"a=inactive\r\n";

	std::ostringstream ss;
	ss << "INVITE sip:" << clientNum << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "From: \"PocketDial\" <sip:pbx@" << srcIpPort << ">;tag=" << fromTag << "\r\n"
	   << "To: <sip:" << clientNum << "@" << activeIp << ">\r\n"
	   << "Call-ID: " << callId << "\r\n"
	   << "CSeq: 1 INVITE\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Contact: <sip:pbx@" << srcIpPort << ";transport=UDP>\r\n"
	   // Auto-answer / intercom headers — identical intent to the 999 all-page fork:
	   // make a Yealink auto-answer in intercom mode and so play its alert tone.
	   << "Call-Info: <sip:any>;answer-after=0\r\n"
	   << "Alert-Info: info=alert-autoanswer\r\n"
	   << "Alert-Info: answer-after=0\r\n"
	   << "Alert-Info: intercom=true\r\n"
	   << "P-Auto-Answer: normal\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Type: application/sdp\r\n"
	   << "Content-Length: " << body.size() << "\r\n\r\n"
	   << body;

	auto invite = getMessageFromPool(ss.str(), addr);
	// Normalise the codec list and (re)derive Content-Length from the actual body —
	// a wrong Content-Length silently breaks the offer on UDP (the 777-path bug).
	invite->enforceG711();
	invite->syncContentLength();
	_outbox.emplace_back(addr, std::move(invite));
	queueLog("Register beep: INVITE -> " + clientNum);
}

std::shared_ptr<SipMessage> RequestsHandler::buildBeepAck(const std::shared_ptr<SipMessage>& ok)
{
	// ACK the phone's final response to our beep INVITE — either the 200 OK (auto-answer
	// path) or a 487 Request Terminated after we CANCELled (#90 teardown path). Same
	// Call-ID/branch/From-tag as the INVITE; To carries the phone's tag from the final
	// response. CSeq stays "1 ACK" (matches the INVITE transaction). No body. Reusing the
	// INVITE branch is exactly right for the non-2xx (487) ACK (RFC 3261 §17.1.1.3).
	BeepDialog* bd = findBeepByCallID(ok->getCallID());
	if (!bd)
	{
		return nullptr;
	}

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &bd->addr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(bd->addr.sin_port));

	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);

	std::ostringstream ss;
	ss << "ACK sip:" << bd->ext << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << bd->branch << "\r\n"
	   << "From: \"PocketDial\" <sip:pbx@" << srcIpPort << ">;tag=" << bd->fromTag << "\r\n"
	   << "To: " << ok->getTo() << "\r\n"
	   << "Call-ID: " << bd->callID << "\r\n"
	   << "CSeq: 1 ACK\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Content-Length: 0\r\n\r\n";

	return getMessageFromPool(ss.str(), bd->addr);
}

std::shared_ptr<SipMessage> RequestsHandler::buildBeepBye(const std::shared_ptr<SipMessage>& ok)
{
	// BYE to end the beep call immediately after the tone. New transaction (fresh
	// branch, CSeq 2 BYE) within the established dialog. We are the UAC here, so
	// From carries the tag we minted on the beep INVITE; To carries the phone's tag.
	BeepDialog* bd = findBeepByCallID(ok->getCallID());
	if (!bd)
	{
		return nullptr;
	}

	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	std::string fromHeader = "\"PocketDial\" <sip:pbx@" + srcIpPort + ">;tag=" + bd->fromTag;

	return buildServerBye(bd->ext, bd->addr, bd->callID, fromHeader, std::string(ok->getTo()));
}

std::shared_ptr<SipMessage> RequestsHandler::buildBeepCancel(std::size_t slot)
{
	// CANCEL the outstanding beep INVITE when the phone never auto-answered. Same
	// Call-ID/branch/From-tag as the INVITE; To has NO tag yet (no final response).
	// CSeq method becomes CANCEL but keeps the INVITE sequence number (RFC 3261 §9.1).
	if (slot >= _beepDialogs.size())
	{
		return nullptr;
	}
	BeepDialog& bd = _beepDialogs[slot];

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &bd.addr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(bd.addr.sin_port));

	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);

	std::ostringstream ss;
	ss << "CANCEL sip:" << bd.ext << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << bd.branch << "\r\n"
	   << "From: \"PocketDial\" <sip:pbx@" << srcIpPort << ">;tag=" << bd.fromTag << "\r\n"
	   << "To: <sip:" << bd.ext << "@" << activeIp << ">\r\n"
	   << "Call-ID: " << bd.callID << "\r\n"
	   << "CSeq: 1 CANCEL\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Content-Length: 0\r\n\r\n";

	return getMessageFromPool(ss.str(), bd.addr);
}

// ── Call parking (park-orbit, roadmap §3.1) ──────────────────────────────────
// Helpers shared by the park methods. Kept file-local (no header surface).

// The dialog tag from a From/To header line (";tag=XXXX"), or "".
static std::string parkTagOf(std::string_view header)
{
	size_t p = header.find(";tag=");
	if (p == std::string_view::npos) return {};
	p += 5;
	size_t e = p;
	while (e < header.size() && header[e] != ';' && header[e] != '>' &&
		header[e] != ' ' && header[e] != '\r' && header[e] != '\n')
	{
		++e;
	}
	return std::string(header.substr(p, e - p));
}

// Strips a leading "Name:" header prefix (e.g. "From:", "To:", "Call-ID:", compact
// "i:"/"f:"/"t:") so server-minted requests carry a clean value and never emit a
// doubled "To: From:" / "Call-ID: Call-ID:". SAFE to call on a bare value too: it
// only strips when the text before the first ':' is a header-name token
// (letters/hyphen) — so a value like "<sip:...>" or "\"PSTN\" <sip:...>" (whose ':'
// is inside the URI) is returned unchanged. Idempotent.
static std::string stripHeaderName(std::string_view h)
{
	size_t colon = h.find(':');
	if (colon == std::string_view::npos || colon == 0 || colon > 15)
		return std::string(h);
	for (size_t i = 0; i < colon; ++i)
	{
		char c = h[i];
		if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '-'))
			return std::string(h);   // not a bare header name → it's a value, leave it
	}
	size_t v = colon + 1;
	while (v < h.size() && (h[v] == ' ' || h[v] == '\t')) ++v;
	size_t e = h.size();
	while (e > v && (h[e - 1] == '\r' || h[e - 1] == '\n')) --e;
	return std::string(h.substr(v, e - v));
}

int RequestsHandler::parkOrbitIndex(std::string_view ext) const
{
	// Orbit extensions "700".."70(N-1)" (N == POCKETDIAL_PARK_SLOTS, ≤ 10).
	if (ext.size() != 3 || ext[0] != '7' || ext[1] != '0') return -1;
	if (ext[2] < '0' || ext[2] > '9') return -1;
	int idx = ext[2] - '0';
	return (idx < static_cast<int>(_parkSlots.size())) ? idx : -1;
}

void RequestsHandler::onParkInvite(std::shared_ptr<SipMessage> data,
	const std::shared_ptr<SipClient>& caller, int orbitIdx)
{
	if (orbitIdx < 0 || orbitIdx >= static_cast<int>(_parkSlots.size()) || !caller)
	{
		return;
	}
	ParkSlot& slot = _parkSlots[orbitIdx];
	const std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	const std::string orbit = "70" + std::to_string(orbitIdx);
	const std::string toTag = IDGen::GenerateID(9);

	if (slot.state == ParkState::Free)
	{
		// ── PARK ── answer the parker with an a=inactive hold SDP — NOT an echo of
		// their own SDP. Echoing their c=/m= back pointed their RTP at themselves
		// (the caller heard their own echo) and left the leg churning; a=inactive is
		// a clean "parked / on hold" answer (silence; the phone shows hold). The
		// parked party's REAL media address is preserved in slot.parkedSdp for the
		// retrieve re-INVITE, so this does not affect retrieval. (Server-sourced MoH
		// for the parked leg is a separate follow-up.)
		const std::string holdSdp =
			"v=0\r\n"
			"o=- 0 0 IN IP4 " + activeIp + "\r\n"
			"s=pocket-dial\r\n"
			"c=IN IP4 " + activeIp + "\r\n"
			"t=0 0\r\n"
			"m=audio 9 RTP/AVP 0\r\n"
			"a=inactive\r\n";
		auto ok = getMessageFromPool(data->toString(), data->getSource());
		ok->setHeader(SipMessageTypes::OK);
		ok->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		ok->setTo(std::string(data->getTo()) + ";tag=" + toTag);
		ok->setContact(buildContact(orbit));
		ok->setBody(holdSdp);
		ok->syncContentLength();
		_outbox.emplace_back(data->getSource(), ok);

		slot = ParkSlot{};
		slot.state      = ParkState::Parked;
		slot.orbit      = orbit;
		slot.callID     = std::string(data->getCallID());   // full "Call-ID: ..." line
		slot.parkedExt  = caller->getNumber();
		slot.parkedAddr = data->getSource();
		// Issue #69: capture only the SDP + From-tag we actually reuse, so the pooled
		// SipMessage is not pinned for the whole park lifetime (M-2 pressure relief).
		slot.parked        = true;
		slot.parkedSdp     = std::string(data->getBody());
		slot.parkedFromTag = parkTagOf(data->getFrom());
		slot.localTag   = toTag;
		slot.parker     = caller->getNumber();
		slot.parkedAt   = std::chrono::steady_clock::now();

		// Pin a Session slot so the parked dialog shows in the dashboard and is
		// torn down on BYE / lease loss like any call.
		auto virt = allocateVirtualPeer(orbit, data->getSource());
		if (auto session = allocateSession(std::string(data->getCallID()), caller))
		{
			session->setDest(virt);
			session->setLocalTag(toTag);
			session->setInviteMessage(data);
			_sessions.emplace(std::string(data->getCallID()), session);
			session->setState(Session::State::Connected);
		}
		queueLog("Park: " + caller->getNumber() + " parked on " + orbit);
		refreshParkSnapshot();
		return;
	}

	// ── RETRIEVE ── the orbit is occupied: bridge the retriever to the parked party
	// peer-to-peer. Answer the retriever with the parked party's SDP, re-INVITE the
	// parked party with the retriever's SDP, link the two legs for BYE bridging, and
	// free the orbit (the call is now a live ordinary bridged call).
	const std::string parkedSdp(slot.parkedSdp);
	const std::string retrieverSdp(data->getBody());

	auto ok = getMessageFromPool(data->toString(), data->getSource());
	ok->setHeader(SipMessageTypes::OK);
	ok->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	ok->setTo(std::string(data->getTo()) + ";tag=" + toTag);
	ok->setContact(buildContact(orbit));
	if (!parkedSdp.empty()) ok->setBody(parkedSdp);
	ok->enforceG711();
	ok->syncContentLength();
	_outbox.emplace_back(data->getSource(), ok);

	sendParkReinvite(slot, retrieverSdp);   // re-point the parked party at the retriever

	// Bridge: a session for the retriever leg, linked to the parked session so a BYE
	// on either relays a server BYE to the other (onBye peer-relay).
	auto virt = allocateVirtualPeer(slot.parkedExt, slot.parkedAddr);
	if (auto rsession = allocateSession(std::string(data->getCallID()), caller))
	{
		rsession->setDest(virt);
		rsession->setPeerCallID(slot.callID);
		rsession->setLocalTag(toTag);
		rsession->setInviteMessage(data);
		_sessions.emplace(std::string(data->getCallID()), rsession);
		rsession->setState(Session::State::Connected);
	}
	if (auto parked = getSession(slot.callID); parked.has_value())
	{
		parked.value()->setPeerCallID(std::string(data->getCallID()));
	}
	queueLog("Park: " + caller->getNumber() + " retrieved " + slot.parkedExt + " from " + orbit);

	slot = ParkSlot{};   // orbit free again
	refreshParkSnapshot();
}

void RequestsHandler::sendParkReinvite(ParkSlot& slot, const std::string& sdp)
{
	if (slot.callID.empty() || !slot.parked) return;
	const std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	const std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &slot.parkedAddr.sin_addr, ipBuf, sizeof(ipBuf));
	const std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(slot.parkedAddr.sin_port));

	// In-dialog re-INVITE: the parked party is the original UAC (their From-tag),
	// we are the UAS (our localTag). As the sender now, From = us (localTag),
	// To = the parked party (their tag). CSeq spaces are per-UA, so 2 is safe.
	const std::string theirTag = slot.parkedFromTag;
	const std::string branch = "z9hG4bK" + IDGen::GenerateID(12);
	const std::string body = sdp;

	std::ostringstream ss;
	ss << "INVITE sip:" << slot.parkedExt << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "From: <sip:" << slot.orbit << "@" << srcIpPort << ">;tag=" << slot.localTag << "\r\n"
	   << "To: <sip:" << slot.parkedExt << "@" << activeIp << ">;tag=" << theirTag << "\r\n"
	   << slot.callID << "\r\n"
	   << "CSeq: 2 INVITE\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Contact: <sip:" << slot.orbit << "@" << srcIpPort << ";transport=UDP>\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Type: application/sdp\r\n"
	   << "Content-Length: " << body.size() << "\r\n\r\n"
	   << body;

	auto inv = getMessageFromPool(ss.str(), slot.parkedAddr);
	inv->enforceG711();
	inv->syncContentLength();
	_outbox.emplace_back(slot.parkedAddr, std::move(inv));
	// We owe an ACK once the parked party 200s this re-INVITE (handleParkOk).
	_parkPendingAcks.push_back(slot.callID);
	queueLog("Park: re-INVITE -> parked party " + slot.parkedExt);
}

void RequestsHandler::byeParkedParty(const ParkSlot& slot)
{
	if (slot.callID.empty()) return;
	const std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	const std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	const std::string theirTag = slot.parkedFromTag;
	// Server-as-UAS BYE (we minted localTag on the 200): From = us+localTag,
	// To = parked party + their tag.
	const std::string fromHeader = "<sip:" + slot.orbit + "@" + srcIpPort + ">;tag=" + slot.localTag;
	const std::string toHeader = "<sip:" + slot.parkedExt + "@" + activeIp + ">;tag=" + theirTag;
	auto bye = buildServerBye(slot.parkedExt, slot.parkedAddr,
		stripHeaderName(slot.callID), fromHeader, toHeader);
	if (bye) _outbox.emplace_back(slot.parkedAddr, std::move(bye));
}

void RequestsHandler::startParkRingback(ParkSlot& slot, const std::shared_ptr<SipClient>& parker,
	std::chrono::steady_clock::time_point now)
{
	if (!parker) { byeParkedParty(slot); freeParkSlot(slot.callID); return; }
	const std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	const std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	const sockaddr_in& addr = parker->getAddress();

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
	const std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));

	// Server-as-UAC INVITE toward the parker carrying the parked party's SDP, so on
	// answer the parker's media points at the parked party; handleParkOk() then
	// ACKs and re-INVITEs the parked party with the parker's SDP to finish the
	// bridge. Fresh dialog (rbCallID/rbFromTag/rbBranch).
	slot.rbCallID  = "Call-ID: " + IDGen::GenerateID(16) + "@" + activeIp;
	slot.rbFromTag = IDGen::GenerateID(9);
	slot.rbBranch  = "z9hG4bK" + IDGen::GenerateID(12);
	slot.rbAddr    = addr;
	slot.state     = ParkState::RingingBack;
	slot.deadline  = now + std::chrono::seconds(30);

	const std::string body = slot.parkedSdp;
	std::ostringstream ss;
	ss << "INVITE sip:" << parker->getNumber() << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << slot.rbBranch << "\r\n"
	   << "From: \"PocketDial Park\" <sip:" << slot.orbit << "@" << srcIpPort << ">;tag=" << slot.rbFromTag << "\r\n"
	   << "To: <sip:" << parker->getNumber() << "@" << activeIp << ">\r\n"
	   << slot.rbCallID << "\r\n"
	   << "CSeq: 1 INVITE\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Contact: <sip:" << slot.orbit << "@" << srcIpPort << ";transport=UDP>\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Type: application/sdp\r\n"
	   << "Content-Length: " << body.size() << "\r\n\r\n"
	   << body;

	auto inv = getMessageFromPool(ss.str(), addr);
	inv->enforceG711();
	inv->syncContentLength();
	_outbox.emplace_back(addr, std::move(inv));
	queueLog("Park: timeout on " + slot.orbit + " — ringing back parker " + parker->getNumber());
}

bool RequestsHandler::handleParkOk(const std::shared_ptr<SipMessage>& data)
{
	const std::string cseq(data->getCSeq());
	const std::string callID(data->getCallID());
	const bool isInviteOk = cseq.find(SipMessageTypes::INVITE) != std::string::npos;
	if (!isInviteOk) return false;

	// (a) 200 OK to a park re-INVITE we sent the parked party (retrieve / ring-back
	//     media re-point): ACK it (server-as-UAC) so the dialog confirms.
	if (auto it = std::find(_parkPendingAcks.begin(), _parkPendingAcks.end(), callID);
		it != _parkPendingAcks.end())
	{
		const std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		const std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
		const sockaddr_in srcAddr = data->getSource();
		char ipBuf[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &srcAddr.sin_addr, ipBuf, sizeof(ipBuf));
		const std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(srcAddr.sin_port));
		std::ostringstream ss;
		ss << "ACK sip:" << data->getToNumber() << "@" << destIpPort << " SIP/2.0\r\n"
		   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=z9hG4bK" << IDGen::GenerateID(12) << "\r\n"
		   // getFrom()/getTo() already include the "From:"/"To:" name — strip it so the
		   // ACK doesn't ship an RFC-illegal doubled prefix (strict UAs discard it).
		   << "From: " << stripHeaderName(data->getFrom()) << "\r\n"
		   << "To: " << stripHeaderName(data->getTo()) << "\r\n"
		   << callID << "\r\n"
		   << "CSeq: 2 ACK\r\n"
		   << "Max-Forwards: 70\r\n"
		   << "Content-Length: 0\r\n\r\n";
		_outbox.emplace_back(data->getSource(), getMessageFromPool(ss.str(), data->getSource()));
		_parkPendingAcks.erase(it);
		return true;
	}

	// (b) 200 OK to a ring-back INVITE we sent the parker: ACK the parker, then
	//     re-INVITE the parked party with the parker's SDP to finish the bridge.
	for (auto& slot : _parkSlots)
	{
		if (slot.state == ParkState::RingingBack && slot.rbCallID == callID)
		{
			const std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
			const std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
			char ipBuf[INET_ADDRSTRLEN]{};
			inet_ntop(AF_INET, &slot.rbAddr.sin_addr, ipBuf, sizeof(ipBuf));
			const std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(slot.rbAddr.sin_port));
			std::ostringstream ss;
			ss << "ACK sip:" << data->getToNumber() << "@" << destIpPort << " SIP/2.0\r\n"
			   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << slot.rbBranch << "\r\n"
			   << "From: \"PocketDial Park\" <sip:" << slot.orbit << "@" << srcIpPort << ">;tag=" << slot.rbFromTag << "\r\n"
			   << "To: " << stripHeaderName(data->getTo()) << "\r\n"   // strip the "To:" name (getTo() includes it)
			   << slot.rbCallID << "\r\n"
			   << "CSeq: 1 ACK\r\n"
			   << "Max-Forwards: 70\r\n"
			   << "Content-Length: 0\r\n\r\n";
			_outbox.emplace_back(slot.rbAddr, getMessageFromPool(ss.str(), slot.rbAddr));

			// Re-point the parked party at the parker (the parker's answer SDP).
			sendParkReinvite(slot, std::string(data->getBody()));

			// Bridge bookkeeping: emplace a Session for the ring-back (parker) leg
			// linked to the parked dialog so a BYE on EITHER side relays a server BYE
			// to the other (onBye peer-relay). Without this link the bridged call
			// would leak on hangup. The ring-back leg is server-as-UAC (setParkUac),
			// and stores the parker's 200 OK so onBye can build the inverted-role BYE.
			if (auto parker = findClient(slot.parker); parker.has_value())
			{
				auto virt = allocateVirtualPeer(slot.parkedExt, slot.parkedAddr);
				if (auto rb = allocateSession(slot.rbCallID, parker.value()))
				{
					rb->setDest(virt);
					rb->setPeerCallID(slot.callID);
					rb->setLocalTag(slot.rbFromTag);
					rb->setParkUac(true);
					rb->setInviteMessage(data);   // the parker's 200 OK (onBye builds the peer BYE from it)
					_sessions.emplace(slot.rbCallID, rb);
					rb->setState(Session::State::Connected);
				}
				if (auto parked = getSession(slot.callID); parked.has_value())
				{
					parked.value()->setPeerCallID(slot.rbCallID);
				}
			}

			queueLog("Park: parker answered ring-back on " + slot.orbit + " — bridging");
			slot = ParkSlot{};   // bridged; orbit released
			refreshParkSnapshot();
			return true;
		}
	}
	return false;
}

void RequestsHandler::parkSweep(std::chrono::steady_clock::time_point now)
{
	for (auto& slot : _parkSlots)
	{
		if (slot.state == ParkState::Parked &&
			(now - slot.parkedAt) >= _parkTimeout)
		{
			// Park expired: ring the parker back if they are registered, else tear
			// the parked leg down so the orbit (and its session slot) is reclaimed.
			auto parker = findClient(slot.parker);
			if (parker.has_value())
			{
				startParkRingback(slot, parker.value(), now);
			}
			else
			{
				queueLog("Park: timeout on " + slot.orbit + " — parker " + slot.parker +
					" gone, tearing down");
				byeParkedParty(slot);
				freeParkSlot(slot.callID);
			}
		}
		else if (slot.state == ParkState::RingingBack && now >= slot.deadline)
		{
			// Parker never answered the ring-back: give up, tear the parked leg down.
			queueLog("Park: ring-back on " + slot.orbit + " not answered — tearing down");
			byeParkedParty(slot);
			freeParkSlot(slot.callID);
		}
	}
	refreshParkSnapshot();
}

void RequestsHandler::freeParkSlot(std::string_view callID)
{
	for (auto& slot : _parkSlots)
	{
		if (slot.state != ParkState::Free && slot.callID == callID)
		{
			slot = ParkSlot{};
		}
	}
	// Also drop any outstanding ACK owed on this dialog.
	_parkPendingAcks.erase(
		std::remove(_parkPendingAcks.begin(), _parkPendingAcks.end(), std::string(callID)),
		_parkPendingAcks.end());
}

void RequestsHandler::refreshParkSnapshot()
{
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> snapLock(_snapshotMutex);
	_snapshot.parked.clear();
	for (const auto& slot : _parkSlots)
	{
		if (slot.state == ParkState::Free) continue;
		int secs = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
			now - slot.parkedAt).count());
		_snapshot.parked.emplace_back(slot.orbit, slot.parkedExt, slot.parker, secs);
	}
}

std::vector<std::tuple<std::string, std::string, std::string, int>> RequestsHandler::getParkedCalls()
{
	std::lock_guard<std::mutex> lock(_snapshotMutex);
	return _snapshot.parked;
}

void RequestsHandler::setParkTimeout(std::chrono::seconds t)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_parkTimeout = t;
}

std::shared_ptr<SipClient> RequestsHandler::allocateClient(std::string number, sockaddr_in address, int expiresSeconds)
{
	// Re-REGISTER: find existing slot by number and refresh it in-place
	for (auto& client : _clientPool)
	{
		if (client->getNumber() == number)
		{
			queueLog("Re-registered: " + number);
			client->reset(number, address, expiresSeconds);
			return client;
		}
	}

	// New client: find the first free slot
	for (auto& client : _clientPool)
	{
		if (client->getNumber().empty())
		{
			queueLog("New Client: " + number);
			client->reset(std::move(number), address, expiresSeconds);
			return client;
		}
	}

	// No free slots! Evict the oldest expired client
	auto now = std::chrono::steady_clock::now();
	for (auto& client : _clientPool)
	{
		if (client->isExpired(now))
		{
			client->reset(std::move(number), address, expiresSeconds);
			return client;
		}
	}

	// Out of space!
	return nullptr;
}

std::shared_ptr<Session> RequestsHandler::allocateSession(std::string callID, std::shared_ptr<SipClient> src)
{
	for (auto& session : _sessionPool)
	{
		const std::string& slotId = session->getCallID();
		if (slotId.empty() || _sessions.find(slotId) == _sessions.end())
		{
			session->reset(std::move(callID), src);
			return session;
		}
	}
	return nullptr;
}

std::shared_ptr<SipClient> RequestsHandler::allocateVirtualPeer(std::string number, sockaddr_in address, int expiresSeconds)
{
	// Issue #70 (L-6): a virtual peer is owned solely by the Session._dest (or a park
	// slot's bridged session) it backs, so a pool slot is free precisely when only the
	// pool itself still references it (use_count()==1). Reset it in place and hand it
	// out — no heap allocation in the handler. Caller holds _mutex.
	for (auto& peer : _virtualPeerPool)
	{
		if (peer.use_count() == 1)
		{
			peer->reset(std::move(number), address, expiresSeconds);
			return peer;
		}
	}
	// Pool drained (more concurrent virtual-ext legs than provisioned): fall back to a
	// one-off heap SipClient rather than failing the call — mirrors the message-pool
	// degradation policy. Bounded by MAX_SESSIONS, so this is a rare safety valve.
	std::cerr << "[WARNING] Virtual-peer pool exhausted! Fallback to heap allocation.\n";
	return std::make_shared<SipClient>(std::move(number), address, expiresSeconds);
}

bool RequestsHandler::ipAllowed(const sockaddr_in& src) const
{
	if (_allowMask == 0) return true; // No allowlist configured
	uint32_t ip = ntohl(src.sin_addr.s_addr);
	return (ip & _allowMask) == _allowNet;
}

bool RequestsHandler::allowPacket(const sockaddr_in& src)
{
	auto now = std::chrono::steady_clock::now();
	uint32_t ip = src.sin_addr.s_addr; // Key by raw network-byte-order IP

	auto it = _rateBuckets.find(ip);
	if (it == _rateBuckets.end())
	{
		if (_rateBuckets.size() >= 256)
		{
			// Fail-safe drop if maximum buckets exceeded
			return false;
		}
		// New bucket: burst 40, sustained 20 pkt/s
		_rateBuckets[ip] = { 40.0, now };
		return true;
	}

	auto& bucket = it->second;
	double elapsedSec = std::chrono::duration<double>(now - bucket.last).count();
	bucket.last = now;

	// Replenish tokens (sustained rate = 20 tokens/sec)
	bucket.tokens = (std::min)(40.0, bucket.tokens + elapsedSec * 20.0);

	if (bucket.tokens >= 1.0)
	{
		bucket.tokens -= 1.0;
		return true;
	}

	return false; // Denied (Rate limit exceeded)
}

bool RequestsHandler::isValidAor(std::string_view s) const
{
	if (s.empty()) return false;
	for (char c : s)
	{
		// Alnum + RFC 3261 user-part punctuation we accept, plus '*' and '#' so star/pound
		// feature codes (e.g. *55) are dialable AORs. Tab/newline stay excluded, which the NVS
		// blob persistence relies on as field/record delimiters.
		if (!std::isalnum(static_cast<unsigned char>(c)) &&
			c != '.' && c != '-' && c != '_' && c != '+' &&
			c != '*' && c != '#')
		{
			return false;
		}
	}
	return true;
}

void RequestsHandler::queueLog(std::string msg, bool isError)
{
	_logQueue.push_back({isError, std::move(msg)});
}

// ── NVS persistence: PBX config (forwards / ring groups) + CDR ring ──────────
//
// All five helpers are no-ops on host (the in-memory maps/ring ARE the store) and
// gate their NVS access on ESP_PLATFORM. Each table is serialized as a single blob
// (one record per line; fields tab-separated) under one NVS key, so a mutation
// rewrites exactly one key — bounded size, low flash wear. Records are bounded by
// POCKETDIAL_MAX_CLIENTS / POCKETDIAL_CDR_RECORDS, so the blob can never grow
// without limit. The AOR charset (isValidAor) excludes tab/newline, so the
// delimiters are safe. Callers hold _mutex (except construction-time loads, which
// run single-threaded before any handler dispatches).

namespace
{
	// Split a serialized blob into newline-delimited records, then each record into
	// tab-separated fields. Pure; used by the loaders.
	std::vector<std::vector<std::string>> deserializeBlob(const std::string& blob)
	{
		std::vector<std::vector<std::string>> records;
		size_t pos = 0;
		while (pos < blob.size())
		{
			size_t nl = blob.find('\n', pos);
			std::string line = blob.substr(pos, (nl == std::string::npos ? blob.size() : nl) - pos);
			pos = (nl == std::string::npos) ? blob.size() : nl + 1;
			if (line.empty()) continue;

			std::vector<std::string> fields;
			size_t fp = 0;
			while (true)
			{
				size_t tab = line.find('\t', fp);
				fields.push_back(line.substr(fp, (tab == std::string::npos ? line.size() : tab) - fp));
				if (tab == std::string::npos) break;
				fp = tab + 1;
			}
			records.push_back(std::move(fields));
		}
		return records;
	}
}

void RequestsHandler::loadPbxConfig()
{
	if (_pbxConfigLoaded)
	{
		return;
	}
	_pbxConfigLoaded = true;

#if defined(ESP_PLATFORM) || defined(ESP32)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) != ESP_OK)
	{
		return;
	}

	auto readBlob = [&](const char* key) -> std::string {
		size_t len = 0;
		if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0)
		{
			return {};
		}
		std::string buf(len, '\0');
		if (nvs_get_str(h, key, buf.data(), &len) != ESP_OK)
		{
			return {};
		}
		if (!buf.empty() && buf.back() == '\0') buf.pop_back(); // drop NUL terminator
		return buf;
	};

	// Forwards: ext \t always \t busy \t noAnswer
	for (const auto& rec : deserializeBlob(readBlob("forwards")))
	{
		if (rec.size() < 4 || rec[0].empty()) continue;
		if (_forwards.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS)) break;
		pbx::ForwardConfig cfg;
		cfg.always = rec[1];
		cfg.busy = rec[2];
		cfg.noAnswer = rec[3];
		if (!cfg.empty()) _forwards[rec[0]] = std::move(cfg);
	}

	// Ring groups: ext \t mode \t m1,m2,...
	for (const auto& rec : deserializeBlob(readBlob("groups")))
	{
		if (rec.size() < 3 || rec[0].empty()) continue;
		if (_ringGroups.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS)) break;
		pbx::RingGroup g;
		g.mode = (rec[1] == "hunt") ? pbx::GroupMode::Hunt : pbx::GroupMode::RingAll;
		g.members = pbx::splitMembers(rec[2]);
		if (!g.members.empty()) _ringGroups[rec[0]] = std::move(g);
	}

	// Paging zones: ext \t m1,m2,...
	for (const auto& rec : deserializeBlob(readBlob("pzones")))
	{
		if (rec.size() < 2 || rec[0].empty()) continue;
		if (!pbx::isPageZoneExt(rec[0])) continue;
		if (_pageZones.size() >= static_cast<size_t>(POCKETDIAL_MAX_PAGE_ZONES)) break;
		pbx::PageZone z;
		z.members = pbx::splitZoneMembers(rec[1]);
		if (!z.members.empty()) _pageZones[rec[0]] = std::move(z);
	}

	nvs_close(h);
#endif
}

// ── Blob serializers (read-only over the in-memory maps; caller holds _mutex) ────
// These were previously inlined in the persistXxx() write-through functions. They
// are now standalone so flushDirtyNvs() can serialize under a SHORT lock and then
// write to flash OUTSIDE the lock (Issue #42 / #55). Pure string work, no NVS — so
// they compile on host too (where they are unused but keep the TU symmetric).
std::string RequestsHandler::serializeForwardsBlob() const
{
	std::string blob;
	for (const auto& [ext, cfg] : _forwards)
	{
		blob += ext; blob += '\t';
		blob += cfg.always; blob += '\t';
		blob += cfg.busy; blob += '\t';
		blob += cfg.noAnswer; blob += '\n';
	}
	return blob;
}

std::string RequestsHandler::serializeRingGroupsBlob() const
{
	std::string blob;
	for (const auto& [ext, g] : _ringGroups)
	{
		blob += ext; blob += '\t';
		blob += (g.mode == pbx::GroupMode::Hunt ? "hunt" : "ringall"); blob += '\t';
		blob += pbx::joinMembers(g.members); blob += '\n';
	}
	return blob;
}

std::string RequestsHandler::serializePageZonesBlob() const
{
	std::string blob;
	for (const auto& [ext, z] : _pageZones)
	{
		blob += ext; blob += '\t';
		blob += pbx::joinMembers(z.members); blob += '\n';
	}
	return blob;
}

// Issue #42/#55: mark-dirty mutators. Called under _mutex; the blocking flash write
// is deferred to flushDirtyNvs() after the lock is released.
void RequestsHandler::persistForwards()   { _nvsDirty |= kNvsDirtyForwards; }
void RequestsHandler::persistRingGroups() { _nvsDirty |= kNvsDirtyRingGroups; }
void RequestsHandler::persistPageZones()  { _nvsDirty |= kNvsDirtyPageZones; }

// ── Registrar mode + device registry persistence (STAGE 2) ───────────────────

void RequestsHandler::loadRegistrarMode()
{
#if defined(ESP_PLATFORM) || defined(ESP32)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) != ESP_OK)
	{
		return;
	}
	uint8_t v = 0;
	esp_err_t err = nvs_get_u8(h, "reg_mode", &v);
	nvs_close(h);
	if (err == ESP_OK && v <= static_cast<uint8_t>(RegistrarMode::Secure))
	{
		_registrarMode.store(static_cast<RegistrarMode>(v), std::memory_order_relaxed);
	}
	// else: keep the compile-time-seeded default (Open under POCKETDIAL_OPEN_REGISTRAR).
#endif
}

void RequestsHandler::persistRegistrarMode()
{
#if defined(ESP_PLATFORM) || defined(ESP32)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) == ESP_OK)
	{
		nvs_set_u8(h, "reg_mode",
			static_cast<uint8_t>(_registrarMode.load(std::memory_order_relaxed)));
		nvs_commit(h);
		nvs_close(h);
	}
#endif
}

// ── #107: anchor TLS re-warm cadence persistence ────────────────────────────────
void RequestsHandler::loadRewarmInterval()
{
#if defined(ESP_PLATFORM) || defined(ESP32)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) != ESP_OK)
	{
		return;
	}
	uint16_t v = 0;
	esp_err_t err = nvs_get_u16(h, "rwm_min", &v);
	nvs_close(h);
	if (err == ESP_OK && v <= 1440)
	{
		_rewarmMinutes.store(v, std::memory_order_relaxed);
	}
	// else: keep the compile-time default (60 min).
#endif
}

void RequestsHandler::persistRewarmInterval()
{
#if defined(ESP_PLATFORM) || defined(ESP32)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) == ESP_OK)
	{
		nvs_set_u16(h, "rwm_min", _rewarmMinutes.load(std::memory_order_relaxed));
		nvs_commit(h);
		nvs_close(h);
	}
#endif
}

void RequestsHandler::loadDevices()
{
#if defined(ESP_PLATFORM) || defined(ESP32)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) != ESP_OK)
	{
		return;
	}
	size_t len = 0;
	if (nvs_get_str(h, "devices", nullptr, &len) == ESP_OK && len > 0)
	{
		std::string buf(len, '\0');
		if (nvs_get_str(h, "devices", buf.data(), &len) == ESP_OK)
		{
			if (!buf.empty() && buf.back() == '\0') buf.pop_back();
			// Record: mac \t extension \t state(int)
			for (const auto& rec : deserializeBlob(buf))
			{
				if (rec.size() < 3 || rec[0].empty()) continue;
				if (_devices.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS)) break;
				DeviceRecord r;
				r.extension = rec[1];
				int si = atoi(rec[2].c_str());
				r.state = (si == static_cast<int>(DeviceState::Secured))
					? DeviceState::Secured : DeviceState::Learned;
				_devices[rec[0]] = std::move(r);
			}
		}
	}
	nvs_close(h);
#endif
}

// mac \t extension \t state(int). Bounded by POCKETDIAL_MAX_CLIENTS, so the blob is
// fixed-footprint. Online state is NOT persisted (it is volatile registration state).
std::string RequestsHandler::serializeDevicesBlob() const
{
	std::string blob;
	for (const auto& [mac, rec] : _devices)
	{
		blob += mac; blob += '\t';
		blob += rec.extension; blob += '\t';
		blob += std::to_string(static_cast<int>(rec.state)); blob += '\n';
	}
	return blob;
}

// Issue #55: mark devices dirty after each adoption / secure / forget. Called under
// _mutex from the Learn/star-code paths; flushed to flash off-lock by flushDirtyNvs().
void RequestsHandler::persistDevices() { _nvsDirty |= kNvsDirtyDevices; }

void RequestsHandler::loadCdrRing()
{
#if defined(ESP_PLATFORM) || defined(ESP32)
	nvs_handle_t h;
	if (nvs_open(NVS_CDR_NS, NVS_READWRITE, &h) != ESP_OK)
	{
		return;
	}
	size_t len = 0;
	if (nvs_get_str(h, "ring", nullptr, &len) == ESP_OK && len > 0)
	{
		std::string buf(len, '\0');
		if (nvs_get_str(h, "ring", buf.data(), &len) == ESP_OK)
		{
			if (!buf.empty() && buf.back() == '\0') buf.pop_back();
			// Record: caller \t callee \t startMs \t durationSec \t result(int)
			for (const auto& rec : deserializeBlob(buf))
			{
				if (rec.size() < 5) continue;
				if (_cdrCount >= POCKETDIAL_CDR_RECORDS) break;
				CallDetailRecord r;
				r.caller = rec[0];
				r.callee = rec[1];
				r.startMs = static_cast<uint64_t>(strtoull(rec[2].c_str(), nullptr, 10));
				r.durationSec = static_cast<uint32_t>(strtoul(rec[3].c_str(), nullptr, 10));
				int ri = atoi(rec[4].c_str());
				r.result = (ri >= 0 && ri <= static_cast<int>(CdrResult::Failed))
					? static_cast<CdrResult>(ri) : CdrResult::Failed;
				// Records were serialized oldest-first; append preserving order.
				_cdrRing[_cdrHead] = std::move(r);
				_cdrHead = (_cdrHead + 1) % POCKETDIAL_CDR_RECORDS;
				++_cdrCount;
			}
		}
	}
	nvs_close(h);
#endif
}

// Serialize the ring oldest-first (same order loadCdrRing replays). Bounded by
// POCKETDIAL_CDR_RECORDS, so the blob is fixed-footprint. Caller holds _mutex.
std::string RequestsHandler::serializeCdrBlob() const
{
	std::string blob;
	for (size_t i = 0; i < _cdrCount; ++i)
	{
		size_t idx = (_cdrHead + POCKETDIAL_CDR_RECORDS - _cdrCount + i) % POCKETDIAL_CDR_RECORDS;
		const CallDetailRecord& r = _cdrRing[idx];
		blob += r.caller; blob += '\t';
		blob += r.callee; blob += '\t';
		blob += std::to_string(r.startMs); blob += '\t';
		blob += std::to_string(r.durationSec); blob += '\t';
		blob += std::to_string(static_cast<int>(r.result)); blob += '\n';
	}
	return blob;
}

// Issue #42: previously this did nvs_open/commit inline — a flash erase/program
// (10–100+ ms) while the caller (endCall, under _mutex) held the registrar lock,
// stalling ALL SIP signaling on every BYE / no-answer teardown. Now it only marks
// the ring dirty; flushDirtyNvs() writes it back from tick() AFTER the lock drops.
// Coalescing also means a burst of teardowns in one tick costs a single flash write.
void RequestsHandler::persistCdrRing() { _nvsDirty |= kNvsDirtyCdr; }

// ── Off-lock NVS write-back (Issue #42 / #55) ────────────────────────────────────
// Called from tick() AFTER the registrar lock has been released. For each dirty
// store it takes a SHORT lock just long enough to snapshot the bit and serialize the
// in-memory blob, then performs the blocking nvs_open/set_str/commit/close OUTSIDE
// any lock. No-op on host (the in-memory maps are the store). If a write fails the
// bit stays cleared for this pass; the next mutation re-arms it (best-effort, matches
// the prior write-through's silent-failure behaviour).
void RequestsHandler::flushDirtyNvs()
{
#if defined(ESP_PLATFORM) || defined(ESP32)
	uint8_t dirty;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		dirty = _nvsDirty;
		_nvsDirty = 0;
	}
	if (dirty == 0) return;

	// Helper: write an already-serialized blob to NVS (outside any lock). const-ref —
	// it only reads the bytes.
	auto writeKey = [](const char* ns, const char* key, const std::string& blob) {
		nvs_handle_t h;
		if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK)
		{
			nvs_set_str(h, key, blob.c_str());
			nvs_commit(h);
			nvs_close(h);
		}
	};

	if (dirty & kNvsDirtyForwards)
	{
		std::string blob;
		{ std::lock_guard<std::mutex> lock(_mutex); blob = serializeForwardsBlob(); }
		writeKey(NVS_PBX_NS, "forwards", blob);
	}
	if (dirty & kNvsDirtyRingGroups)
	{
		std::string blob;
		{ std::lock_guard<std::mutex> lock(_mutex); blob = serializeRingGroupsBlob(); }
		writeKey(NVS_PBX_NS, "groups", blob);
	}
	if (dirty & kNvsDirtyPageZones)
	{
		std::string blob;
		{ std::lock_guard<std::mutex> lock(_mutex); blob = serializePageZonesBlob(); }
		writeKey(NVS_PBX_NS, "pzones", blob);
	}
	if (dirty & kNvsDirtyDevices)
	{
		std::string blob;
		{ std::lock_guard<std::mutex> lock(_mutex); blob = serializeDevicesBlob(); }
		writeKey(NVS_PBX_NS, "devices", blob);
	}
	if (dirty & kNvsDirtyCdr)
	{
		std::string blob;
		{ std::lock_guard<std::mutex> lock(_mutex); blob = serializeCdrBlob(); }
		writeKey(NVS_CDR_NS, "ring", blob);
	}
#endif
}

// ── Task 2B: Admin extension NVS key ─────────────────────────────────────────

void RequestsHandler::loadAdminExt()
{
#if defined(ESP_PLATFORM) || defined(ESP32)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) != ESP_OK)
	{
		return;
	}
	char buf[32] = {0};
	size_t len = sizeof(buf);
	esp_err_t err = nvs_get_str(h, "admin_ext", buf, &len);
	nvs_close(h);
	if (err == ESP_OK && buf[0] != '\0')
	{
		_adminExt = buf;
	}
	// else: keep the in-class default "101"
#endif
}

void RequestsHandler::saveAdminExt(const std::string& ext)
{
	_adminExt = ext;
#if defined(ESP_PLATFORM) || defined(ESP32)
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) == ESP_OK)
	{
		nvs_set_str(h, "admin_ext", ext.c_str());
		nvs_commit(h);
		nvs_close(h);
	}
#endif
}

std::string RequestsHandler::getAdminExt() const
{
	return _adminExt;
}

// ── Task 2C: DTMF digit-collection state machine + CLASS service codes ────────

void RequestsHandler::onDtmfInfo(std::shared_ptr<SipMessage> data)
{
	// --- 1. Parse "Signal=X" from the body -----------------------------------
	const std::string& raw = data->toString();
	char digit = 0;
	{
		size_t sep = raw.find("\r\n\r\n");
		if (sep == std::string::npos) sep = raw.find("\n\n");
		if (sep != std::string::npos)
		{
			std::string body = raw.substr(sep);
			size_t sigPos = body.find("Signal=");
			if (sigPos == std::string::npos) sigPos = body.find("signal=");
			if (sigPos != std::string::npos)
			{
				size_t valIdx = sigPos + 7; // after "Signal="
				while (valIdx < body.size() && body[valIdx] == ' ') ++valIdx;
				if (valIdx < body.size())
				{
					digit = body[valIdx];
				}
			}
		}
	}
	if (digit == 0)
	{
		return; // malformed / no signal — nothing to do
	}

	// --- 2. Look up or create the per-Call-ID accumulator -------------------
	std::string callId(data->getCallID());
	auto& accum = _dtmfState[callId];

	// --- 3. Timeout: reset accumulator if > TIMEOUT_MS since last digit -----
#if defined(ESP_PLATFORM) || defined(ESP32)
	TickType_t now = xTaskGetTickCount();
	uint32_t elapsedMs = (now - accum.lastTick) * portTICK_PERIOD_MS;
#else
	uint32_t now = static_cast<uint32_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	uint32_t elapsedMs = (accum.lastTick == 0) ? 0 : (now - accum.lastTick);
#endif
	if (accum.lastTick != 0 && elapsedMs > DtmfAccum::TIMEOUT_MS)
	{
		accum.digits.clear();
	}
	accum.lastTick = now;

	// --- 4. Append digit ----------------------------------------------------
	accum.digits += digit;
	const std::string& seq = accum.digits;
	std::string callerExt(data->getFromNumber());

	// --- 5. Admin menu gate (Task 2C-5): *PIN + 3-digit code ----------------
	// Pattern: * + PIN(4+) + 3-digit-code  (minimum 8 chars total after '*')
	// Admin gate fires only when the caller IS the admin extension.
	if (callerExt == _adminExt && !seq.empty() && seq[0] == '*')
	{
		// Format: '*' + PIN(>=4 digits) + '#' + 3-digit code [+ confirm digit].
		// The '#' terminates the PIN so its length is unambiguous: we verify the
		// PIN EXACTLY ONCE per completed code. (The old version looped over every
		// candidate PIN length calling verifyPin() for each, so a single normal
		// admin entry charged several failed attempts against the brute-force
		// lockout and could lock the admin out of both DTMF and the dashboard.)
		bool adminMatched = false;
		size_t hashPos = seq.find('#');
		if (hashPos != std::string::npos && hashPos >= 5 && (seq.size() - hashPos - 1) >= 3)
		{
			std::string pinCandidate = seq.substr(1, hashPos - 1);
			std::string rest = seq.substr(hashPos + 1);   // CODE[confirm]
			std::string code = rest.substr(0, 3);
			// PIN must be all digits.
			bool allDigits = !pinCandidate.empty();
			for (char c : pinCandidate)
			{
				if (!std::isdigit(static_cast<unsigned char>(c))) { allDigits = false; break; }
			}
			// Single verify — a wrong PIN is exactly one counted failed attempt.
			// Issue #57: account DTMF brute-force on its own channel so a fat-fingered
			// (or hostile) in-call PIN entry can't lock the admin out of the dashboard.
			if (!allDigits || !AdminAuth::verifyPin(pinCandidate, AdminAuth::Channel::Dtmf))
			{
				queueLog("[admin] DTMF admin auth failed", true);
				accum.digits.clear();
				return;
			}

			// PIN verified — execute the command code.
			if (code == "001")
			{
				// NTP resync (esp_sntp_restart is ESP-IDF v5+; fall back to log if absent)
#if defined(ESP_PLATFORM) || defined(ESP32)
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
				esp_sntp_restart();
#else
				queueLog("[admin] NTP sync requested via DTMF (esp_sntp_restart not available on this IDF version)");
#endif
#endif
				queueLog("[admin] NTP sync requested via DTMF");
				adminMatched = true;
			}
			else if (code == "101")
			{
				// Topology switch: toggle wifi_mode between 1 (CLIENT) and 2 (AP).
#if defined(ESP_PLATFORM) || defined(ESP32)
				nvs_handle_t h;
				if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK)
				{
					uint8_t mode = 1;
					nvs_get_u8(h, "wifi_mode", &mode);
					mode = (mode == 1) ? 2 : 1;
					nvs_set_u8(h, "wifi_mode", mode);
					nvs_commit(h);
					nvs_close(h);
				}
				queueLog("[admin] topology switch via DTMF, restarting");
				esp_restart();
#else
				queueLog("[admin] topology switch requested (stub on host)");
#endif
				adminMatched = true;
			}
			else if (code == "200")
			{
				// Extension target config stub
				queueLog("[admin] targets config: dial new ext (stub)");
				adminMatched = true;
			}
			else if (code == "999")
			{
				// Factory reset — requires a follow-up confirm digit '1'.
				if (rest.size() >= 4)
				{
					if (rest[3] == '1')
					{
						queueLog("[admin] factory reset confirmed via DTMF");
#if defined(ESP_PLATFORM) || defined(ESP32)
						nvs_flash_erase();
						esp_restart();
#else
						queueLog("[admin] factory reset (stub on host)");
#endif
					}
					else
					{
						queueLog("[admin] factory reset aborted (confirm != '1')");
					}
					adminMatched = true;
				}
				else
				{
					// Confirm digit not yet received — keep the accumulator and
					// wait. Do NOT set adminMatched (the tail would clear it).
					queueLog("[admin] factory reset: awaiting confirm digit '1'");
					return;
				}
			}

			if (adminMatched)
			{
				accum.digits.clear();
				return;
			}
		}

		// If the sequence starts with *NNNN (4+ digits) but no code matched yet,
		// and the wrong caller is trying, send 403.
	}
	else if (callerExt != _adminExt && !seq.empty() && seq[0] == '*' &&
	         seq.find('#') != std::string::npos)
	{
		// A non-admin caller attempting the admin-menu pattern (*PIN#…): reject.
		// CLASS service codes (*60/*72/…) have no '#', so they fall through to the
		// per-subscriber feature handling below for any registered caller.
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader("SIP/2.0 403 Forbidden");
		response->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		accum.digits.clear();
		return;
	}

	// --- 6. CLASS feature code matching (Task 2C-4) --------------------------

	// *60 — Enable Selective Call Rejection (DND=true) for caller's extension.
	if (seq == "*60")
	{
		// isDndEnabled / setDnd operate on the _dnd map. We're already inside _mutex.
		if (_dnd.find(callerExt) == _dnd.end() &&
			_dnd.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
		{
			queueLog("*60 SCR: DND table full for " + callerExt, true);
		}
		else
		{
			_dnd[callerExt] = true;
			queueLog("*60 SCR enabled for " + callerExt);
		}
		accum.digits.clear();
		return;
	}

	// *80 — Disable SCR/DND for caller's extension.
	if (seq == "*80")
	{
		_dnd.erase(callerExt);
		queueLog("*80 SCR disabled for " + callerExt);
		accum.digits.clear();
		return;
	}

	// *73 — Disable CFU for caller's extension.
	if (seq == "*73")
	{
		auto it = _forwards.find(callerExt);
		if (it != _forwards.end())
		{
			it->second.always.clear();
			if (it->second.empty()) _forwards.erase(it);
			persistForwards();
		}
		queueLog("*73 CFU disabled for " + callerExt);
		accum.digits.clear();
		return;
	}

	// *69 — Speak last-caller extension: redirect call to echo ext 777 and log CDR lookup.
	if (seq == "*69")
	{
		// Find the last CDR entry where callee == callerExt (i.e. last inbound call).
		std::string lastCaller;
		for (size_t i = 0; i < _cdrCount; ++i)
		{
			size_t idx = (_cdrHead + POCKETDIAL_CDR_RECORDS - 1 - i) % POCKETDIAL_CDR_RECORDS;
			if (_cdrRing[idx].callee == callerExt && !_cdrRing[idx].caller.empty())
			{
				lastCaller = _cdrRing[idx].caller;
				break;
			}
		}
		if (!lastCaller.empty())
		{
			queueLog("*69 last caller for " + callerExt + " is " + lastCaller);
			// Reroute to extension 777 (echo loopback) so the caller hears tones.
			// Find the active session for this Call-ID and redirect its RTP to 777.
			auto session = getSession(callId);
			if (session.has_value())
			{
				auto virtualClient = allocateVirtualPeer("777", session.value()->getSrc()
					? session.value()->getSrc()->getAddress() : sockaddr_in{});
				session.value()->setDest(virtualClient);
			}
		}
		else
		{
			queueLog("*69 no last caller found for " + callerExt);
		}
		accum.digits.clear();
		return;
	}

	// *11 — Echo loopback: reroute active call's RTP endpoint to extension 777.
	if (seq == "*11")
	{
		auto session = getSession(callId);
		if (session.has_value())
		{
			auto src = session.value()->getSrc();
			if (src)
			{
				auto virtualClient = allocateVirtualPeer("777", src->getAddress());
				session.value()->setDest(virtualClient);
				queueLog("*11 echo loopback for call " + callId);
			}
		}
		accum.digits.clear();
		return;
	}

	// *72NNNN — Enable CFU for caller's extension to NNNN (4+ digits after *72).
	// Requires the full sequence to be collected; we match once it's ≥6 chars and
	// none of the above shorter patterns matched.
	if (seq.size() >= 6 && seq[0] == '*' && seq[1] == '7' && seq[2] == '2')
	{
		std::string target = seq.substr(3);
		if (target.size() >= 4 && isValidAor(target))
		{
			bool isNew = (_forwards.find(callerExt) == _forwards.end());
			if (isNew && _forwards.size() >= static_cast<size_t>(POCKETDIAL_MAX_CLIENTS))
			{
				queueLog("*72 CFU: forward table full for " + callerExt, true);
			}
			else
			{
				_forwards[callerExt].always = target;
				persistForwards();
				queueLog("*72 CFU enabled: " + callerExt + " -> " + target);
			}
			accum.digits.clear();
			return;
		}
		// else: keep accumulating (target not yet 4 digits)
	}
}

RequestsHandler::TrunkConfig RequestsHandler::getTrunkConfig()
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _trunkCfg;
}

bool RequestsHandler::isTrunkConnected()
{
	// AnchorClient::isConnected() takes no RequestsHandler lock; safe off-thread.
	return _anchorClient && _anchorClient->isConnected();
}

std::string RequestsHandler::setTrunkConfig(const TrunkConfig& cfg)
{
	if (!cfg.useLoopback)
	{
		if (cfg.baseUrl.rfind("https://", 0) != 0)
		{
			return "Base URL must start with https://";
		}
		if (cfg.clientId.empty() || cfg.clientSecret.empty() || cfg.sourceDn.empty())
		{
			return "Client ID, secret and source DN are all required";
		}
	}

#if defined(ESP_PLATFORM) || defined(ESP32)
	nvs_handle_t h;
	if (nvs_open("storage", NVS_READWRITE, &h) != ESP_OK)
	{
		return "NVS open failed";
	}
	bool ok = nvs_set_str(h, "3cx_base_url", cfg.baseUrl.c_str()) == ESP_OK &&
	          nvs_set_str(h, "3cx_client_id", cfg.clientId.c_str()) == ESP_OK &&
	          nvs_set_str(h, "3cx_secret", cfg.clientSecret.c_str()) == ESP_OK &&
	          nvs_set_str(h, "3cx_source_dn", cfg.sourceDn.c_str()) == ESP_OK &&
	          nvs_set_u8(h, "3cx_loopback", cfg.useLoopback ? 1 : 0) == ESP_OK;
	ok = ok && nvs_commit(h) == ESP_OK;
	nvs_close(h);
	if (!ok)
	{
		return "NVS write failed";
	}
#endif

	std::lock_guard<std::mutex> lock(_mutex);
	_trunkCfg = cfg;
	queueLog("[3CX] Trunk config saved (applies on next reboot)");
	return "";
}

std::vector<TelephonyApiConfig::SlotView> RequestsHandler::getTelephonyApis()
{
	std::lock_guard<std::mutex> lock(_mutex);
	std::vector<TelephonyApiConfig::SlotView> out;
	out.reserve(TelephonyApiConfig::kSlots);
	for (size_t i = 0; i < TelephonyApiConfig::kSlots; ++i)
	{
		out.push_back(_tapiCfg.view(i));   // display-safe: secret never crosses
	}
	return out;
}

std::string RequestsHandler::setTelephonyApi(size_t idx, const TelephonyApiConfig::Slot& s, bool keepSecret)
{
	std::lock_guard<std::mutex> lock(_mutex);
	std::string err = _tapiCfg.setSlot(idx, s, keepSecret);
	if (err.empty())
	{
		// Log the slot + provider only — NEVER credential values.
		queueLog("[TAPI] Slot " + std::to_string(idx) + " saved (" +
		         telephonyProviderName(s.type) + ") — applies on next reboot");
	}
	return err;
}

std::string RequestsHandler::clearTelephonyApi(size_t idx)
{
	std::lock_guard<std::mutex> lock(_mutex);
	std::string err = _tapiCfg.clearSlot(idx);
	if (err.empty())
	{
		queueLog("[TAPI] Slot " + std::to_string(idx) + " cleared");
	}
	return err;
}

std::string RequestsHandler::setTelephonyApiActive(size_t idx)
{
	std::lock_guard<std::mutex> lock(_mutex);
	std::string err = _tapiCfg.setActiveSlot(idx);
	if (err.empty())
	{
		queueLog("[TAPI] Active slot set — applies on next reboot");
	}
	return err;
}

void RequestsHandler::loadThreeCxConfig()
{
	std::string baseUrl;
	std::string clientId;
	std::string clientSecret;
	std::string sourceDn;
	uint8_t useLoopback = 0;

#if defined(ESP_PLATFORM) || defined(ESP32)
	nvs_handle_t h;
	if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK)
	{
		char buf[128] = {0};
		size_t len = sizeof(buf);
		if (nvs_get_str(h, "3cx_base_url", buf, &len) == ESP_OK && buf[0] != '\0')
		{
			baseUrl = buf;
		}

		std::memset(buf, 0, sizeof(buf));
		len = sizeof(buf);
		if (nvs_get_str(h, "3cx_client_id", buf, &len) == ESP_OK && buf[0] != '\0')
		{
			clientId = buf;
		}

		std::memset(buf, 0, sizeof(buf));
		len = sizeof(buf);
		// NVS keys are capped at 15 chars (NVS_KEY_NAME_MAX_SIZE-1); "3cx_secret"
		// and "3cx_loopback" stay under that. The longer "3cx_client_secret" /
		// "3cx_use_loopback" forms silently fail nvs_get with KEY_TOO_LONG.
		if (nvs_get_str(h, "3cx_secret", buf, &len) == ESP_OK && buf[0] != '\0')
		{
			clientSecret = buf;
		}

		std::memset(buf, 0, sizeof(buf));
		len = sizeof(buf);
		if (nvs_get_str(h, "3cx_source_dn", buf, &len) == ESP_OK && buf[0] != '\0')
		{
			sourceDn = buf;
		}

		nvs_get_u8(h, "3cx_loopback", &useLoopback);
		nvs_close(h);
	}
	if (baseUrl.empty())
	{
		useLoopback = 1; // no credentials provisioned — stay in loopback until configured
	}
#else
	useLoopback = 1;
#endif

	// Mirror what we loaded into the config snapshot the SSH trunk screen reads.
	// Construction is single-threaded, so no lock is needed here.
	_trunkCfg.baseUrl = baseUrl;
	_trunkCfg.clientId = clientId;
	_trunkCfg.clientSecret = clientSecret;
	_trunkCfg.sourceDn = sourceDn;
	_trunkCfg.useLoopback = (useLoopback != 0);

	// ── Provider registry (fixed-size factory, boot-constructed instances) ────
	// Construction is single-threaded; the registry is read-only afterwards.
	_providerRegistry.registerProvider(TelephonyProviderType::Loopback, &_loopbackClient);
	_providerRegistry.registerProvider(TelephonyProviderType::ThreeCx, &_threeCxClient);
	_providerRegistry.registerProvider(TelephonyProviderType::Apidaze, &_stubApidaze);
	_providerRegistry.registerProvider(TelephonyProviderType::VoipInnovations, &_stubVoipInnovations);
	_providerRegistry.registerProvider(TelephonyProviderType::Sangoma, &_stubSangoma);

	// ── Telephony-API credential slots (new config domain, "tapicfg") ─────────
	_tapiCfg.load();

	// Resolve the boot provider TYPE. Default = the legacy trunk decision
	// (loopback unless live 3CX creds are provisioned — byte-for-byte the old
	// behavior). An ACTIVE+ENABLED Telephony-API slot overrides it; a slot
	// pointing at an unimplemented (stub) provider falls back to loopback with
	// an honest log instead of pretending to dial.
	TelephonyProviderType bootType = useLoopback ? TelephonyProviderType::Loopback
	                                             : TelephonyProviderType::ThreeCx;
	std::string tapiUrl, tapiId, tapiSecret, tapiDn;
	bool credsFromSlot = false;
	{
		const size_t act = _tapiCfg.activeSlot();
		const TelephonyApiConfig::Slot* slot = _tapiCfg.bootSlot(act);
		if (slot != nullptr && slot->enabled)
		{
			if (telephonyProviderImplemented(slot->type))
			{
				bootType = slot->type;
				tapiUrl = slot->baseUrl; tapiId = slot->clientId;
				tapiSecret = slot->secret; tapiDn = slot->routeDn;
				credsFromSlot = (slot->type != TelephonyProviderType::Loopback);
			}
			else
			{
				queueLog(std::string("[TAPI] Active provider ") +
				         telephonyProviderName(slot->type) +
				         " is not implemented yet — falling back to loopback", true);
				bootType = TelephonyProviderType::Loopback;
			}
		}
	}

	// Init credentials: a Telephony-API slot supplies its own; the legacy
	// "storage"/"3cx_*" keys remain the source otherwise (zero behavior change).
	if (credsFromSlot)
	{
		_threeCxClient.init(tapiUrl, tapiId, tapiSecret, tapiDn);
		_loopbackClient.init(tapiUrl, tapiId, tapiSecret, tapiDn);
	}
	else
	{
		_threeCxClient.init(baseUrl, clientId, clientSecret, sourceDn);
		_loopbackClient.init(baseUrl, clientId, clientSecret, sourceDn);
	}
	// Drop the local plaintext copy now the client holds it (best-effort).
	std::fill(tapiSecret.begin(), tapiSecret.end(), '\0');

#if !defined(ESP_PLATFORM) && !defined(ESP32)
	// Host builds never run a live upstream client (no TLS/WebSocket stack in
	// the host binary) — same force-to-loopback the old code applied.
	if (bootType == TelephonyProviderType::ThreeCx)
	{
		bootType = TelephonyProviderType::Loopback;
		queueLog("[3CX] Config: Using loopback mock client (host build force)");
	}
#endif

	_anchorClient = _providerRegistry.select(bootType);
	if (_anchorClient == nullptr || !telephonyProviderImplemented(bootType))
	{
		_anchorClient = &_loopbackClient;   // registry is total today; belt+braces
		bootType = TelephonyProviderType::Loopback;
	}
	if (bootType == TelephonyProviderType::Loopback)
	{
		queueLog("[3CX] Config: Using loopback mock client");
	}
	else
	{
		queueLog(std::string("[3CX] Config: Using real ") +
		         telephonyProviderName(bootType) + " client on " +
		         (credsFromSlot ? tapiUrl : baseUrl));
	}

	// #107: push the persisted TLS re-warm cadence into the freshly-selected anchor (no-op on
	// loopback). _rewarmMinutes was loaded from NVS earlier in this ctor; minutes -> seconds.
	_anchorClient->setRewarmIntervalSec(
		static_cast<uint32_t>(_rewarmMinutes.load(std::memory_order_relaxed)) * 60u);

	_mediaBridge.init(&_rtpReceiver, &_rtpSender, _anchorClient);

	// Start the anchor client asynchronously to avoid blocking the constructor with TLS fetchToken
#if defined(ESP_PLATFORM) || defined(ESP32)
	struct StartClientArg {
		AnchorClient* anchor;
		RequestsHandler* handler;
	};
	auto* arg = new StartClientArg{ _anchorClient, this };
	xTaskCreate([](void* p) {
		auto* sca = static_cast<StartClientArg*>(p);
		if (sca->anchor->start())
		{
			std::lock_guard<std::mutex> lock(sca->handler->_mutex);
			sca->handler->queueLog("[3CX] Anchor client started");
		}
		else
		{
			std::lock_guard<std::mutex> lock(sca->handler->_mutex);
			sca->handler->queueLog("[3CX] Failed to start anchor client", true);
		}
		delete sca;
		vTaskDelete(NULL);
		// 12288: start() runs a full mbedTLS handshake (fetchToken) + WS connect;
		// 4096 overflowed the instant the real client was used (bootloop on hw).
	}, "3cx_start", 12288, arg, 5, NULL);
#else
	// Member thread, joined in ~RequestsHandler — a detached thread here captured
	// `this` and outlived the handler in unit tests (nondeterministic segfaults).
	_anchorStartThread = std::thread([this]() {
		if (_anchorClient->start())
		{
			std::lock_guard<std::mutex> lock(_mutex);
			queueLog("[3CX] Anchor client started");
		}
		else
		{
			std::lock_guard<std::mutex> lock(_mutex);
			queueLog("[3CX] Failed to start anchor client", true);
		}
	});
#endif

	// Set event callback
	_anchorClient->setEventCallback([this](const AnchorClient::CallEvent& ev) {
		std::lock_guard<std::mutex> lock(_mutex);
		if (ev.type == AnchorClient::CallEvent::Answered)
		{
			queueLog("[3CX] Event: Answered, participantId=" + ev.participantId);
			// Find the active ringing session.
			for (auto& [callId, session] : _sessions)
			{
				if (session->getState() == Session::State::Invited && session->isAnchor())
				{
					// Found the session!
					auto caller = session->getSrc();
					if (caller)
					{
						std::string handsetIp;
						uint16_t handsetPort = 0;
						auto inviteMsg = session->getInviteMessage();
						if (inviteMsg && parseCallerRtp(inviteMsg, handsetIp, handsetPort))
						{
							// Send 200 OK back to handset
							std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
							// Reuse the To-tag from the 180 Ringing (stored on the session) so
							// the 200 OK lands in the SAME dialog the phone is already ringing;
							// a fresh tag here is why the Yealink kept ringing through the answer.
							std::string toTag = session->getLocalTag();
							if (toTag.empty()) toTag = IDGen::GenerateID(9); // defensive fallback
							// Start the MediaBridge FIRST so the receiver's ephemeral port
							// (what the handset must send its audio to) is known before we
							// build the SDP answer. Advertising the sender's source port (the
							// old behaviour) sent the handset's audio into a socket nothing
							// read — the far end heard silence.
							if (!_mediaBridge.startBridge(handsetIp, handsetPort, callId, ev.participantId))
							{
								queueLog("[3CX] MediaBridge failed to start", true);
								break;
							}
							int rxPort = _mediaBridge.receiverPort();
							queueLog("[3CX] MediaBridge started: Handset=" + handsetIp + ":" +
							         std::to_string(handsetPort) + " <-> 3CX (rx port " + std::to_string(rxPort) + ")");
							std::string sdpBody = buildMediaSdp(activeIp, rxPort, /*sendRecv=*/true);

							auto ok = buildOkWithSdp(inviteMsg, activeIp, toTag, sdpBody);
							dumpWire("[3CX] 200 OK ->", ok);

							// This callback runs on the WS event task, NOT the SIP receive
							// thread — use _asyncOutbox so the start-of-body _outbox.clear() in
							// handle()/tick() can't wipe the 200 OK before it is sent.
							_asyncOutbox.emplace_back(inviteMsg->getSource(), std::move(ok));
							session->setState(Session::State::Connected);
							// Remember the upstream (3CX) participant so the handset's BYE
							// can drop it via REST. The OUTBOUND session was never given a
							// participant id before (only inbound was), so asyncDropCall("")
							// fell back to an empty _activeParticipantId → no drop → the PSTN
							// leg lingered until 3CX timed out (or was killed by hand).
							session->setAnchorParticipantId(ev.participantId);
						}
					}
					break;
				}
			}
		}
		else if (ev.type == AnchorClient::CallEvent::Incoming)
		{
			queueLog("[3CX] Event: Incoming, participantId=" + ev.participantId +
			         (ev.callerId.empty() ? "" : (", caller=" + ev.callerId)));
			routeInboundAnchorCall(ev.participantId, ev.callerId);
		}
		else if (ev.type == AnchorClient::CallEvent::Dropped)
		{
			queueLog("[3CX] Event: Dropped, participantId=" + ev.participantId);
			// Find the active session and terminate it
			for (auto& [callId, session] : _sessions)
			{
				if (!session->isAnchor()) continue;

				_mediaBridge.stopBridge();
				std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
				std::string localTag = session->getLocalTag();
				if (localTag.empty()) localTag = IDGen::GenerateID(9); // defensive fallback

				if (session->isAnchorInbound())
				{
					// INBOUND: we are the UAC toward the handset (dest). If the handset
					// already ANSWERED (Connected, we hold its tag) → BYE it; if it is
					// still RINGING → CANCEL our outstanding INVITE instead. From carries
					// OUR tag; there is no handset INVITE to mine, so tags come from the
					// session. All sends use _asyncOutbox (this runs off the SIP thread).
					auto handset = session->getDest();
					if (handset && session->getState() == Session::State::Connected &&
					    !session->getRemoteTag().empty())
					{
						std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
						std::string srcNum = session->getSrc() ? session->getSrc()->getNumber() : std::string("PSTN");
						std::string fromHeader = "\"" + srcNum + "\" <sip:" + handset->getNumber() +
						                         "@" + srcIpPort + ">;tag=" + localTag;
						std::string toHeader = "<sip:" + handset->getNumber() + "@" + activeIp +
						                       ">;tag=" + session->getRemoteTag();
						auto bye = buildServerBye(handset->getNumber(), handset->getAddress(), callId,
						                          fromHeader, toHeader);
						if (bye) _asyncOutbox.emplace_back(handset->getAddress(), std::move(bye));
					}
					else
					{
						// Still ringing — no winner yet. CANCEL every outstanding forked leg.
						for (const auto& target : session->getPendingTargets())
						{
							auto cancel = buildInboundCancelTo(session, target);
							if (cancel) _asyncOutbox.emplace_back(target->getAddress(), std::move(cancel));
						}
					}
					std::string localCallId = callId;
					endCall(localCallId, ev.participantId, handset ? handset->getNumber() : "", "3CX hangup (inbound)");
					break;
				}

				// OUTBOUND: server is UAS; BYE the original caller (src), From carries the
				// To-tag we minted on the 180/200. Without it tag-strict handsets (Yealink)
				// reject the BYE and stay off-hook on a dead call (Issue #12).
				auto caller = session->getSrc();
				auto inviteMsg = session->getInviteMessage();
				if (caller && inviteMsg)
				{
					std::string fromHeader = "<sip:" + std::string(inviteMsg->getToNumber()) +
					                         "@" + activeIp + ">;tag=" + localTag;
					auto bye = buildServerBye(caller->getNumber(), caller->getAddress(), callId,
					                          fromHeader, std::string(inviteMsg->getFrom()));
					if (bye)
					{
						_asyncOutbox.emplace_back(caller->getAddress(), std::move(bye)); // WS task — see _asyncOutbox
					}
				}

				// Copy callId to a local std::string before calling endCall to avoid dangling reference
				std::string localCallId = callId;
				endCall(localCallId, session->getSrc() ? session->getSrc()->getNumber() : "", inviteMsg ? inviteMsg->getToNumber() : "", "3CX hangup");
				break;
			}
		}
	});
}

void RequestsHandler::dumpWire(const char* label, const std::shared_ptr<SipMessage>& msg)
{
	if (!msg) return;
	queueLog(std::string(label) + "\n" + msg->toString());
}

void RequestsHandler::dumpWire(const char* label, std::string_view method, std::string_view callID, std::string_view to)
{
	queueLog(std::string(label) + std::string(method) + " callID=" + std::string(callID) + " to=" + std::string(to));
}

std::shared_ptr<SipMessage> RequestsHandler::buildOkWithSdp(
	const std::shared_ptr<SipMessage>& inviteMsg,
	const std::string& activeIp,
	const std::string& toTag,
	const std::string& sdpBody)
{
	auto ok = getMessageFromPool(inviteMsg->toString(), inviteMsg->getSource());
	ok->setHeader(SipMessageTypes::OK);
	ok->setVia(std::string(inviteMsg->getVia()) + ";received=" + activeIp);
	ok->setTo(std::string(inviteMsg->getTo()) + ";tag=" + toTag);
	ok->setContact(buildContact(inviteMsg->getToNumber()));
	ok->clearBody();
	{
		std::string raw = ok->toString();
		size_t sep = raw.find("\r\n\r\n");
		if (sep != std::string::npos)
		{
			std::string_view headerView(raw.data(), sep);
			if (headerView.find("application/sdp") == std::string_view::npos)
			{
				raw.insert(sep, "\r\nContent-Type: application/sdp");
				sep = raw.find("\r\n\r\n");
			}
			raw.erase(sep + 4);
			raw += sdpBody;
		}
		ok->reset(std::move(raw), inviteMsg->getSource());
	}
	ok->enforceG711();
	ok->syncContentLength();
	return ok;
}

std::shared_ptr<SipMessage> RequestsHandler::buildServerBye(
	const std::string& destExt,
	const sockaddr_in& destAddr,
	const std::string& callId,
	const std::string& fromHeader,
	const std::string& toHeader)
{
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &destAddr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(destAddr.sin_port));
	std::string branch = "z9hG4bK" + IDGen::GenerateID(12);

	// Callers pass either bare values or full "Name: value" header lines (e.g. some
	// hand getInviteMessage()->getFrom() / the _sessions map key, which include the
	// name). Strip any leading header name so we never emit a doubled "To: From:" /
	// "Call-ID: Call-ID:" that tag-strict phones (Yealink) reject — the cause of the
	// handset staying off-hook on a dead trunk call (Issue #12). stripHeaderName is a
	// no-op on bare values.
	std::ostringstream ss;
	ss << "BYE sip:" << destExt << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "From: " << stripHeaderName(fromHeader) << "\r\n"
	   << "To: " << stripHeaderName(toHeader) << "\r\n"
	   << "Call-ID: " << stripHeaderName(callId) << "\r\n"
	   << "CSeq: 2 BYE\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Content-Length: 0\r\n\r\n";

	return getMessageFromPool(ss.str(), destAddr);
}

void RequestsHandler::sendRinging(
	const std::shared_ptr<SipMessage>& data,
	const std::string& activeIp,
	const std::string& toTag,
	const char* logPrefix)
{
	auto ringing = getMessageFromPool(data->toString(), data->getSource());
	ringing->setHeader(SipMessageTypes::RINGING);
	ringing->setVia(std::string(data->getVia()) + ";received=" + activeIp);
	ringing->setTo(std::string(data->getTo()) + ";tag=" + toTag);
	ringing->setContact(buildContact(data->getToNumber()));
	ringing->clearBody();
	ringing->syncContentLength();

	std::string label = std::string(logPrefix) + " 180 Ringing ->";
	dumpWire(label.c_str(), ringing);

	_outbox.emplace_back(data->getSource(), std::move(ringing));
}

void RequestsHandler::routeAnchorCall(const std::shared_ptr<SipMessage>& data,
                                      const std::shared_ptr<SipClient>& caller,
                                      const std::string& dialed)
{
	// Check if we already have an active external call/bridge
	if (_mediaBridge.isActive())
	{
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
		responseObj->setHeader("SIP/2.0 486 Busy Here");
		responseObj->clearBody();
		responseObj->setContact(buildContact(caller->getNumber()));
		endHandle(data->getFromNumber(), responseObj);
		return;
	}

	// Allocate session
	auto newSession = allocateSession(std::string(data->getCallID()), caller);
	if (!newSession)
	{
		std::shared_ptr<SipMessage> responseObj = getMessageFromPool(data->toString(), data->getSource());
		responseObj->setHeader("SIP/2.0 503 Service Unavailable");
		responseObj->clearBody();
		responseObj->setContact(buildContact(caller->getNumber()));
		endHandle(data->getFromNumber(), responseObj);
		return;
	}

	auto virtualClient = allocateVirtualPeer(dialed, data->getSource());
	newSession->setDest(virtualClient);
	newSession->setInviteMessage(data);
	newSession->setState(Session::State::Invited);
	newSession->setAnchor(true); // mark as WAN-anchor session — see Session::isAnchor()
	_sessions.emplace(data->getCallID(), newSession);

	// Send "180 Ringing" back to the handset. Generate the dialog To-tag ONCE
	// here and store it on the session; the 200 OK (sent later from the 3CX
	// Answered callback) MUST reuse this exact tag, or Yealink-class phones
	// treat the 200 as a foreign dialog and never leave the ringing state.
	std::string localTag = IDGen::GenerateID(9);
	newSession->setLocalTag(localTag);
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	sendRinging(data, activeIp, localTag, "[3CX]");

	// Trigger outbound 3CX call asynchronously to avoid blocking the main SIP thread
	asyncMakeCall(dialed, std::string(data->getCallID()), caller->getNumber());
}

void RequestsHandler::routeInboundAnchorCall(const std::string& participantId, const std::string& callerId)
{
	// Runs under _mutex (the anchor event callback holds it). The monitored DN (sourceDn) is
	// a 3CX route point, not a phone, so we RING-ALL: fork an offerless INVITE to every
	// registered extension (server-as-UAC, the register-beep machinery), first answer wins.
	// sourceDn is only the gate that says this anchor is configured to take inbound at all.
	const std::string dn = _trunkCfg.sourceDn;
	if (dn.empty())
	{
		queueLog("[3CX] Inbound: no sourceDn configured — dropping participant " + participantId, true);
		asyncDropCall(participantId);
		return;
	}

	// One media bridge ⇒ one trunk-bridged call at a time (the documented media cap).
	if (_mediaBridge.isActive())
	{
		queueLog("[3CX] Inbound: media bridge busy — dropping inbound to " + dn);
		asyncDropCall(participantId);
		return;
	}

	// Refuse a second inbound while one is already ringing/up (keyed by the inbound flag).
	for (const auto& [cid, s] : _sessions)
	{
		if (s->isAnchorInbound() && s->getState() != Session::State::Bye)
		{
			queueLog("[3CX] Inbound: a call is already in progress — dropping new inbound");
			asyncDropCall(participantId);
			return;
		}
	}

	// RING-ALL: the monitored DN (sourceDn) is a 3CX route point, not a registered phone,
	// so fork the inbound call to EVERY registered extension. First to answer wins
	// (onInboundAnchorOk); the rest are CANCELled. Gather the live registrar set — we hold
	// _mutex, so reading _clientPool directly is safe (an empty number ⇒ free pool slot).
	std::vector<std::shared_ptr<SipClient>> targets;
	for (const auto& client : _clientPool)
	{
		if (!client->getNumber().empty())
		{
			targets.push_back(client);
		}
	}
	if (targets.empty())
	{
		queueLog("[3CX] Inbound: no extensions registered — dropping participant " + participantId, true);
		asyncDropCall(participantId);
		return;
	}

	// Sanitised caller label for the From display-name (quoted token: no '"'/CR/LF).
	std::string callerDisplay = callerId.empty() ? std::string("PSTN") : callerId;
	callerDisplay.erase(std::remove_if(callerDisplay.begin(), callerDisplay.end(),
		[](char c){ return c == '"' || c == '\r' || c == '\n'; }), callerDisplay.end());
	if (callerDisplay.empty()) callerDisplay = "PSTN";

	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	// Key the session by the FULL header-line form ("Call-ID: <id>") so the handsets' 200 OK /
	// 486 / 487 responses — which getSession() looks up via SipMessage::getCallID() (which
	// returns the whole line, name included) — match. The forked INVITEs carry the bare <id>
	// as the header value (stripHeaderName on the getter), which the phones echo back verbatim.
	std::string callId  = std::string("Call-ID: ") + IDGen::GenerateID(16) + "@" + activeIp;
	std::string branch  = "z9hG4bK" + IDGen::GenerateID(12);
	std::string fromTag = IDGen::GenerateID(9);

	// One session backs the whole fork: a synthetic PSTN src (zeroed address — only its label
	// feeds getSrc() snapshots/CDR, never routed/looked-up), with all forked legs sharing the
	// Call-ID / Via branch / From-tag so each is a single cancellable transaction toward its
	// phone. dest stays unset until a winner answers; pendingTargets holds the ringing legs.
	sockaddr_in pstnAddr{};
	pstnAddr.sin_family = AF_INET;
	auto pstn = allocateVirtualPeer(callerDisplay, pstnAddr);
	auto session = allocateSession(callId, pstn);
	if (!session)
	{
		queueLog("[3CX] Inbound: session pool exhausted — dropping", true);
		asyncDropCall(participantId);
		return;
	}
	session->setAnchor(true);
	session->setAnchorInbound(true);
	session->setState(Session::State::Invited);
	session->setLocalTag(fromTag);
	session->setUacBranch(branch);
	session->setAnchorParticipantId(participantId);
	session->setPendingTargets(targets);
	session->armRingTimer(std::chrono::steady_clock::now() + NO_ANSWER_TIMEOUT);
	_sessions.emplace(callId, session);

	// Delayed-offer INVITE per target (no SDP): the single-start MediaBridge can't advertise
	// a port before it binds, and the winning handset's 200 OK carries its offer — our ACK
	// answers it (onInboundAnchorOk). No auto-answer headers: the extensions should RING.
	for (const auto& target : targets)
	{
		buildInboundInviteFork(session, target, callerDisplay);
	}
	queueLog("[3CX] Inbound: ringing " + std::to_string(targets.size()) +
	         " extension(s) for participant " + participantId);
}

void RequestsHandler::buildInboundInviteFork(const std::shared_ptr<Session>& session,
	const std::shared_ptr<SipClient>& target,
	const std::string& callerDisplay)
{
	// One delayed-offer INVITE toward `target`, reusing the session's shared Call-ID / Via
	// branch / From-tag. The dialog's From/To/Contact user is the target's own DN (matched
	// verbatim when it answers/cancels); the display name presents the PSTN caller.
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	const std::string dn = target->getNumber();

	const sockaddr_in& addr = target->getAddress();
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));

	std::ostringstream ss;
	ss << "INVITE sip:" << dn << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << session->getUacBranch() << "\r\n"
	   << "From: \"" << callerDisplay << "\" <sip:" << dn << "@" << srcIpPort << ">;tag=" << session->getLocalTag() << "\r\n"
	   << "To: <sip:" << dn << "@" << activeIp << ">\r\n"
	   << "Call-ID: " << stripHeaderName(session->getCallID()) << "\r\n"   // getCallID() returns the full line
	   << "CSeq: 1 INVITE\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Contact: <sip:" << dn << "@" << srcIpPort << ";transport=UDP>\r\n"
	   << "User-Agent: pocket-dial\r\n"
	   << "Content-Length: 0\r\n\r\n";

	auto invite = getMessageFromPool(ss.str(), addr);
	invite->syncContentLength();
	// The event callback runs OFF the SIP receive thread — _asyncOutbox survives the
	// per-pass _outbox.clear() in handle()/tick() (same rule as the WS 200 OK/BYE).
	_asyncOutbox.emplace_back(addr, std::move(invite));
}

std::shared_ptr<SipMessage> RequestsHandler::buildInboundCancelTo(const std::shared_ptr<Session>& session,
	const std::shared_ptr<SipClient>& target)
{
	if (!target) return nullptr;

	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	std::string dn = target->getNumber();
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &target->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(target->getAddress().sin_port));
	// The fork INVITE's From display is the PSTN caller label, which is exactly the virtual
	// src peer's number — so getSrc()->getNumber() reproduces it byte-for-byte for the match.
	std::string srcNum = session->getSrc() ? session->getSrc()->getNumber() : std::string("PSTN");

	// CANCEL matches the forked INVITE transaction at `target`: identical Request-URI, top
	// Via branch (shared by the whole fork), From (+tag), To (no tag — no final response was
	// accepted), Call-ID, and CSeq number with method CANCEL.
	std::ostringstream cs;
	cs << "CANCEL sip:" << dn << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << session->getUacBranch() << "\r\n"
	   << "From: \"" << srcNum << "\" <sip:" << dn << "@" << srcIpPort << ">;tag=" << session->getLocalTag() << "\r\n"
	   << "To: <sip:" << dn << "@" << activeIp << ">\r\n"
	   << "Call-ID: " << stripHeaderName(session->getCallID()) << "\r\n"   // getCallID() returns the full line
	   << "CSeq: 1 CANCEL\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Content-Length: 0\r\n\r\n";
	return getMessageFromPool(cs.str(), target->getAddress());
}

std::shared_ptr<SipMessage> RequestsHandler::buildInboundCancel(const std::shared_ptr<Session>& session)
{
	// Back-compat single-leg wrapper: CANCEL the session's current dest (the answered/answering
	// handset). Ring-all loser teardown uses buildInboundCancelTo() directly per target.
	return buildInboundCancelTo(session, session->getDest());
}

void RequestsHandler::ackInboundFinal(const std::shared_ptr<Session>& session, const std::shared_ptr<SipMessage>& data)
{
	// RFC 3261 §17.1.1.3: ACK a non-2xx final within the INVITE transaction. Same top Via
	// branch as the forked INVITE (the shared fork branch), To from the response (carries the
	// leg's tag), CSeq 1 ACK. The responding phone is the packet source.
	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	std::string dn(data->getToNumber());
	const sockaddr_in& addr = data->getSource();
	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));
	std::string srcNum = session->getSrc() ? session->getSrc()->getNumber() : std::string("PSTN");

	std::ostringstream ack;
	ack << "ACK sip:" << dn << "@" << destIpPort << " SIP/2.0\r\n"
	    << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << session->getUacBranch() << "\r\n"
	    << "From: \"" << srcNum << "\" <sip:" << dn << "@" << srcIpPort << ">;tag=" << session->getLocalTag() << "\r\n"
	    << "To: " << stripHeaderName(data->getTo()) << "\r\n"
	    << "Call-ID: " << stripHeaderName(session->getCallID()) << "\r\n"   // getCallID() returns the full line
	    << "CSeq: 1 ACK\r\n"
	    << "Max-Forwards: 70\r\n"
	    << "Content-Length: 0\r\n\r\n";
	_outbox.emplace_back(addr, getMessageFromPool(ack.str(), addr));
}

void RequestsHandler::onInboundAnchorOk(const std::shared_ptr<SipMessage>& ok, const std::shared_ptr<Session>& session)
{
	// The handset answered our INVITE (server-as-UAC). Learn its tag + RTP, bring up the
	// media bridge, ACK with our SDP answer (delayed-offer model), then answer upstream.
	// Runs on the SIP receive thread under _mutex.
	session->clearRingTimer();

	std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
	std::string srcIpPort = activeIp + ":" + std::to_string(_serverPort);
	std::string callId(session->getCallID());          // full line — the _sessions key (endCall/buildServerBye take it)
	std::string callIdHdr = stripHeaderName(callId);   // bare value for the Call-ID headers we emit on the wire

	// RING-ALL: dest is unset until a forked leg answers. The answering phone IS the OK's
	// source address, so resolve it (mirrors the broadcast winner lookup); fall back to a
	// previously-bound dest for the winner's 200 OK retransmits.
	auto answering = findClientByAddress(ok->getSource());
	std::shared_ptr<SipClient> handset = answering.has_value() ? answering.value() : session->getDest();
	if (!handset)
	{
		return;   // can't identify the answering extension — ignore
	}

	// Loser-race: a DIFFERENT extension answered after another leg already won and bridged
	// (its 200 OK crossed our CANCEL). RFC 3261 §9 — we must ACK then BYE that orphan 2xx,
	// but must NOT disturb the live bridge or the upstream leg. Reject it and return.
	if (session->getState() == Session::State::Connected &&
	    session->getDest() && handset->getNumber() != session->getDest()->getNumber())
	{
		char obuf[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &handset->getAddress().sin_addr, obuf, sizeof(obuf));
		std::string orphanIpPort = std::string(obuf) + ":" + std::to_string(ntohs(handset->getAddress().sin_port));
		std::string orphanFrom = "\"" + (session->getSrc() ? session->getSrc()->getNumber() : std::string("PSTN")) +
		                         "\" <sip:" + handset->getNumber() + "@" + srcIpPort + ">;tag=" + session->getLocalTag();
		std::string orphanAckBranch = "z9hG4bK" + IDGen::GenerateID(12);
		std::ostringstream oack;
		oack << "ACK sip:" << handset->getNumber() << "@" << orphanIpPort << " SIP/2.0\r\n"
		     << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << orphanAckBranch << "\r\n"
		     << "From: " << orphanFrom << "\r\n"
		     << "To: " << ok->getTo() << "\r\n"
		     << "Call-ID: " << callIdHdr << "\r\n"
		     << "CSeq: 1 ACK\r\n"
		     << "Max-Forwards: 70\r\n"
		     << "Content-Length: 0\r\n\r\n";
		_outbox.emplace_back(handset->getAddress(), getMessageFromPool(oack.str(), handset->getAddress()));
		auto obye = buildServerBye(handset->getNumber(), handset->getAddress(), callId, orphanFrom,
		                           std::string(ok->getTo()));
		if (obye) _outbox.emplace_back(handset->getAddress(), std::move(obye));
		queueLog("[3CX] Inbound: extra answer from " + handset->getNumber() + " — rejected (call already up)");
		return;
	}

	// First answer wins: bind this phone as the dialog's dest for the rest of the call.
	if (!session->getDest())
	{
		session->setDest(handset);
	}

	// Handset's To-tag — needed on every in-dialog request we now send (ACK, BYE).
	std::string toHdr(ok->getTo());
	std::string remoteTag;
	size_t tp = toHdr.find(";tag=");
	if (tp != std::string::npos)
	{
		remoteTag = toHdr.substr(tp + 5);
		size_t e = remoteTag.find_first_of(";> \r\n\t");
		if (e != std::string::npos) remoteTag.erase(e);
	}
	session->setRemoteTag(remoteTag);

	// Rebuild the dialog's local From verbatim (display = src label, user = DN, our tag)
	// so ACK/BYE match the INVITE. To = the phone's full To header (carries its tag).
	std::string fromHeader = "\"" + (session->getSrc() ? session->getSrc()->getNumber() : std::string("PSTN")) +
	                         "\" <sip:" + handset->getNumber() + "@" + srcIpPort + ">;tag=" + session->getLocalTag();

	char ipBuf[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &handset->getAddress().sin_addr, ipBuf, sizeof(ipBuf));
	std::string destIpPort = std::string(ipBuf) + ":" + std::to_string(ntohs(handset->getAddress().sin_port));

	// Idempotency: the phone retransmits its 200 OK until it sees our ACK. The FIRST one
	// (still Invited) brings up the bridge and answers upstream; a retransmit arriving
	// after we are Connected must ONLY re-emit the ACK (with the same answer) — never
	// start a second bridge (which would fail and tear the live call down).
	const bool already = (session->getState() == Session::State::Connected);
	std::string handsetIp; uint16_t handsetPort = 0;
	std::string sdpAnswer;
	bool bridged = false;
	if (already)
	{
		if (_mediaBridge.isActive())
		{
			sdpAnswer = buildMediaSdp(activeIp, _mediaBridge.receiverPort(), /*sendRecv=*/true);
			bridged = true;   // re-ACK only; bridge already up
		}
	}
	else if (parseCallerRtp(ok, handsetIp, handsetPort) &&
	         _mediaBridge.startBridge(handsetIp, handsetPort, callId, session->getAnchorParticipantId()))
	{
		int rxPort = _mediaBridge.receiverPort();
		sdpAnswer = buildMediaSdp(activeIp, rxPort, /*sendRecv=*/true);
		bridged = true;
		queueLog("[3CX] Inbound: handset " + handsetIp + ":" + std::to_string(handsetPort) +
		         " answered; bridge up (rx " + std::to_string(rxPort) + ")");
	}
	else
	{
		queueLog("[3CX] Inbound: no usable SDP / bridge failed — tearing down", true);
	}

	// ACK the 2xx (RFC 3261 §13.2.2.4): a NEW transaction branch, CSeq 1 ACK. Carries
	// our SDP answer on success; on failure it is sent bodyless purely to quench the
	// phone's 200 retransmits before we BYE it.
	std::string ackBranch = "z9hG4bK" + IDGen::GenerateID(12);
	std::ostringstream ack;
	ack << "ACK sip:" << handset->getNumber() << "@" << destIpPort << " SIP/2.0\r\n"
	    << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << ackBranch << "\r\n"
	    << "From: " << fromHeader << "\r\n"
	    << "To: " << ok->getTo() << "\r\n"
	    << "Call-ID: " << callIdHdr << "\r\n"
	    << "CSeq: 1 ACK\r\n"
	    << "Max-Forwards: 70\r\n";
	if (bridged && !sdpAnswer.empty())
	{
		ack << "Content-Type: application/sdp\r\n"
		    << "Content-Length: " << sdpAnswer.size() << "\r\n\r\n"
		    << sdpAnswer;
	}
	else
	{
		ack << "Content-Length: 0\r\n\r\n";
	}
	_outbox.emplace_back(handset->getAddress(), getMessageFromPool(ack.str(), handset->getAddress()));

	if (already)
	{
		return;   // retransmitted 200 OK: ACK re-sent, nothing else to do
	}

	if (!bridged)
	{
		// Couldn't bridge: hang up both legs. BYE the handset (new CSeq 2 transaction),
		// drop the upstream participant, end the session.
		auto bye = buildServerBye(handset->getNumber(), handset->getAddress(), callId, fromHeader,
		                          std::string(ok->getTo()));
		if (bye) _outbox.emplace_back(handset->getAddress(), std::move(bye));
		asyncDropCall(session->getAnchorParticipantId());
		endCall(callId, session->getSrc() ? session->getSrc()->getNumber() : "", handset->getNumber(),
		        "inbound bridge failed");
		return;
	}

	session->setState(Session::State::Connected);
	// Tell the upstream to connect the PSTN leg; its Connected upsert opens the PCM
	// streams (startMediaStreams) so audio flows handset RTP ⇄ bridge ⇄ PCM ⇄ PSTN.
	asyncAnswerCall(session->getAnchorParticipantId());

	// RING-ALL: this leg won — CANCEL every other still-ringing fork so the other phones
	// stop. The winner is now dest; the rest live in pendingTargets. Clear it afterward so
	// the no-answer / PSTN-abandon teardown paths don't re-CANCEL an already-answered call.
	for (const auto& target : session->getPendingTargets())
	{
		if (target->getNumber() != handset->getNumber())
		{
			auto cancel = buildInboundCancelTo(session, target);
			if (cancel) _outbox.emplace_back(target->getAddress(), std::move(cancel));
		}
	}
	session->setPendingTargets({});
}

void RequestsHandler::asyncMakeCall(const std::string& destination, const std::string& callId, const std::string& callerNumber)
{
	if (!_anchorClient) return;
#if defined(ESP_PLATFORM) || defined(ESP32)
	struct MakeCallArg {
		AnchorClient* anchor;
		std::string dest;
		std::string callId;
		std::string callerNumber;
		RequestsHandler* handler;
	};
	auto* arg = new MakeCallArg{ _anchorClient, destination, callId, callerNumber, this };
	xTaskCreate([](void* p) {
		auto* mca = static_cast<MakeCallArg*>(p);
		if (!mca->anchor->makeCall(mca->dest))
		{
			std::lock_guard<std::mutex> lock(mca->handler->_mutex);
			mca->handler->queueLog("[3CX] Failed to initiate outbound call to " + mca->dest, true);
			mca->handler->endCall(mca->callId, mca->callerNumber, mca->dest, "3CX call fail");
		}
		else
		{
			std::lock_guard<std::mutex> lock(mca->handler->_mutex);
			mca->handler->queueLog("[3CX] Initiating outbound call to " + mca->dest);
		}
		delete mca;
		vTaskDelete(NULL);
		// 12288: makeCall is a TLS HTTPS round trip — same overflow as 3cx_start.
	}, "3cx_makecall", 12288, arg, 5, NULL);
#else
	spawnAnchorWorker([this, destination, callId, callerNumber]() {
		if (!_anchorClient->makeCall(destination))
		{
			std::lock_guard<std::mutex> lock(_mutex);
			queueLog("[3CX] Failed to initiate outbound call to " + destination, true);
			endCall(callId, callerNumber, destination, "3CX call fail");
		}
		else
		{
			std::lock_guard<std::mutex> lock(_mutex);
			queueLog("[3CX] Initiating outbound call to " + destination);
		}
	});
#endif
}

void RequestsHandler::asyncDropCall(const std::string& participantId)
{
	if (!_anchorClient) return;
#if defined(ESP_PLATFORM) || defined(ESP32)
	struct DropCallArg {
		AnchorClient* anchor;
		std::string partId;
		RequestsHandler* handler;
	};
	auto* arg = new DropCallArg{ _anchorClient, participantId, this };
	// 12288: dropCall is a TLS HTTPS round trip — same overflow as 3cx_start.
	// #94: CHECK the spawn. Under heap pressure during an active call the 12 KB stack can
	// fail to allocate; the drop worker then never runs and the PSTN leg never tears down.
	// This used to be silent — surface it (and free the arg) so the failure is visible in
	// the serial log instead of a phantom non-drop.
	if (xTaskCreate([](void* p) {
		auto* dca = static_cast<DropCallArg*>(p);
		dca->anchor->dropCall(dca->partId);
		delete dca;
		vTaskDelete(NULL);
	}, "3cx_dropcall", 12288, arg, 5, NULL) != pdPASS)
	{
		queueLog("[3CX] asyncDropCall: drop worker xTaskCreate FAILED (heap exhausted) — leg NOT dropped", true);
		delete arg;
	}
#else
	spawnAnchorWorker([this, participantId]() {
		_anchorClient->dropCall(participantId);
	});
#endif
}

void RequestsHandler::asyncAnswerCall(const std::string& participantId)
{
	// Answer an inbound upstream participant off the SIP thread (the POST is a TLS round
	// trip). Mirrors asyncDropCall's threading/lifetime rules exactly.
	if (!_anchorClient) return;
#if defined(ESP_PLATFORM) || defined(ESP32)
	struct AnswerCallArg {
		AnchorClient* anchor;
		std::string partId;
		RequestsHandler* handler;
	};
	auto* arg = new AnswerCallArg{ _anchorClient, participantId, this };
	xTaskCreate([](void* p) {
		auto* aca = static_cast<AnswerCallArg*>(p);
		if (!aca->anchor->answerCall(aca->partId))
		{
			std::lock_guard<std::mutex> lock(aca->handler->_mutex);
			aca->handler->queueLog("[3CX] Failed to answer inbound participant " + aca->partId, true);
		}
		delete aca;
		vTaskDelete(NULL);
		// 12288: answerCall is a TLS HTTPS round trip — same overflow as 3cx_start.
	}, "3cx_answer", 12288, arg, 5, NULL);
#else
	spawnAnchorWorker([this, participantId]() {
		if (!_anchorClient->answerCall(participantId))
		{
			std::lock_guard<std::mutex> lock(_mutex);
			queueLog("[3CX] Failed to answer inbound participant " + participantId, true);
		}
	});
#endif
}

#if !defined(ESP_PLATFORM) && !defined(ESP32)
// Issue #67: spawn a host anchor worker that runs `job` then flips its done-flag.
// The flag lets reapAnchorWorkers() join+erase finished threads in tick(), so the
// live thread count tracks in-flight calls rather than growing without bound. We
// reap opportunistically here too, so a steady call rate never lets the vector grow.
void RequestsHandler::spawnAnchorWorker(std::function<void()> job)
{
	auto done = std::make_shared<std::atomic<bool>>(false);
	std::thread worker([job = std::move(job), done]() {
		job();
		done->store(true, std::memory_order_release);
	});
	std::lock_guard<std::mutex> lock(_anchorWorkMutex);
	// Reap already-finished siblings before appending (bounds the vector under churn).
	reapFinishedLocked();
	_anchorWorkThreads.push_back(AnchorWorker{ std::move(worker), std::move(done) });
}

// Caller MUST hold _anchorWorkMutex. Join + erase every worker whose done-flag is set.
void RequestsHandler::reapFinishedLocked()
{
	for (auto it = _anchorWorkThreads.begin(); it != _anchorWorkThreads.end(); )
	{
		if (it->done && it->done->load(std::memory_order_acquire))
		{
			if (it->thread.joinable()) it->thread.join();
			it = _anchorWorkThreads.erase(it);
		}
		else
		{
			++it;
		}
	}
}

// Called from tick(): reap finished anchor workers. With drainAll, blocks until every
// worker has finished (used by the destructor as a belt-and-suspenders join).
void RequestsHandler::reapAnchorWorkers(bool drainAll)
{
	std::vector<AnchorWorker> toJoin;
	{
		std::lock_guard<std::mutex> lock(_anchorWorkMutex);
		if (drainAll)
		{
			toJoin = std::move(_anchorWorkThreads);
			_anchorWorkThreads.clear();
		}
		else
		{
			reapFinishedLocked();
			return;
		}
	}
	// drainAll: join outside the lock so a still-running worker that needs _anchorWorkMutex
	// (it doesn't today, but be safe) can't deadlock us.
	for (auto& w : toJoin)
	{
		if (w.thread.joinable()) w.thread.join();
	}
}
#endif

