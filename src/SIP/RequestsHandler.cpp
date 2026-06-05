// RequestsHandler.cpp: Issues #24 and #28 resolved.
#include "RequestsHandler.hpp"
#include <atomic>
#include <sstream>
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

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	// PBX config (call-forward / ring groups) and the persistent CDR ring live in
	// NVS on the device. nvs_flash/nvs are core ESP-IDF components present on every
	// transport, so they gate on ESP_PLATFORM (not POCKETDIAL_HAS_WIFI). On host the
	// in-memory maps/ring ARE the store and these calls compile out (see the
	// load*/persist* helpers at the bottom of this file).
	#include "nvs_flash.h"
	#include "nvs.h"
#endif

std::vector<std::shared_ptr<SipMessage>> RequestsHandler::_messagePool;

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
	if (_messagePool.empty())
	{
		_messagePool.reserve(POCKETDIAL_MSG_POOL);
		for (int i = 0; i < POCKETDIAL_MSG_POOL; ++i)
		{
			_messagePool.push_back(std::make_shared<SipSdpMessage>("", sockaddr_in{}));
		}
	}
	_dummyClient = std::make_shared<SipClient>();

	// Reload persisted PBX config (call-forward / ring groups) and the CDR ring from
	// NVS so they survive reboot. No-ops on host. Construction is single-threaded
	// (no handler is dispatching yet), so these run without holding _mutex.
	loadPbxConfig();
	loadCdrRing();
}

std::shared_ptr<SipMessage> RequestsHandler::getMessageFromPool(std::string message, sockaddr_in src)
{
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

		// Surface inbound client-error (4xx) responses once. Deferred via _logQueue
		// so the write happens outside the lock (Issue #24); replaces the per-parse
		// printf the parser used to do. softFail -> warning (stdout), else error (stderr).
		if (status.has_value() && status->klass == PocketDial::SipStatusClass::ClientError)
		{
			queueLog("[SIP] " + std::string(status->softFail ? "WARN " : "ERROR ")
				+ std::string(request->getHeader()), !status->softFail);
		}

		auto it = _handlers.find(handlerKey);
		if (it != _handlers.end())
		{
			it->second(std::move(request));
		}
		localOutbox = std::move(_outbox);
		_outbox.clear();

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

#ifndef POCKETDIAL_OPEN_REGISTRAR
	// Closed mode: reject registrations as unauthenticated/forbidden since we don't have registered credentials.
	{
		auto response = getMessageFromPool(data->toString(), data->getSource());
		response->setHeader("SIP/2.0 403 Forbidden");
		response->clearBody();
		std::string activeIp = (_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp;
		response->setVia(std::string(data->getVia()) + ";received=" + activeIp);
		_outbox.emplace_back(data->getSource(), std::move(response));
		return;
	}
#endif

	if (requestedExpires <= 0)
	{
		// expires=0 (or an explicit zero) is a de-registration request.
		unregisterClient(fromNumber);
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
	if (destNumber == "777")
	{
		endCall(data->getCallID(), data->getFromNumber(), "777");
		return;
	}

	if (destNumber == "999")
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
		endCall(data->getCallID(), data->getFromNumber(), "999");
		return;
	}

	setCallState(data->getCallID(), Session::State::Cancel);
	endHandle(data->getToNumber(), data);
}

void RequestsHandler::onReqTerminated(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isBroadcast())
	{
		return;
	}
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onInvite(std::shared_ptr<SipMessage> data)
{
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

		_dummyClient->reset("777", data->getSource(), 3600);
		auto newSession = allocateSession(std::string(data->getCallID()), caller.value());
		if (newSession)
		{
			newSession->setDest(_dummyClient);
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

	// Check if the called is registered
	auto called = findClient(data->getToNumber());
	if (!called.has_value())
	{
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

void RequestsHandler::onTrying(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	if (session.has_value() && session.value()->isBroadcast())
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
			endCall(data->getCallID(), session.value()->getSrc()->getNumber(), "999", "all targets busy");
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
			endCall(data->getCallID(), session.value()->getSrc()->getNumber(), "999", "all targets unavailable");
		}
		return;
	}
	setCallState(data->getCallID(), Session::State::Unavailable);
	endHandle(data->getFromNumber(), data);
}

void RequestsHandler::onBye(std::shared_ptr<SipMessage> data)
{
	auto session = getSession(data->getCallID());
	std::string destNumber(data->getToNumber());
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

	if (destNumber == "999")
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

	auto session = getSession(data->getCallID());
	if (session.has_value())
	{
		if (session.value()->getState() == Session::State::Cancel)
		{
			endHandle(data->getFromNumber(), data);
			return;
		}

		if (data->getCSeq().find(SipMessageTypes::INVITE) != std::string::npos) 
		{
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

	std::string destNumber(data->getToNumber());
	if (destNumber == "777")
	{
		return;
	}

	if (destNumber == "999")
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

	// Tear down the transferor's existing dialog (blind transfer drops the original
	// call) so a stale session doesn't pin a pool slot, then drive the new INVITE.
	std::string callID(data->getCallID());

	auto targetClient = findClient(target);
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

	// Drop the transferor's prior leg (best-effort; CDR recorded as it tears down).
	endCall(callID, transferor->getNumber(), std::string(data->getToNumber()), "blind transfer");
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
	std::vector<std::shared_ptr<SipClient>> targets,
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
	_sessions.emplace(invite->getCallID(), newSession);

	std::string contactExt = intercom ? std::string("999") : std::string(invite->getToNumber());

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
		case Session::State::Busy:        return "Busy";
		case Session::State::Unavailable: return "Unavailable";
		case Session::State::Cancel:      return "Cancel";
		case Session::State::Bye:         return "Bye";
		default:                          return "Unknown";
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
		if (extension == "777" || extension == "999")
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

		if (groupExt == "777" || groupExt == "999")
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

void RequestsHandler::tick()
{
	auto now = std::chrono::steady_clock::now();
	if (now - _lastTick < std::chrono::seconds(1))
	{
		return;
	}
	_lastTick = now;

	std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> localOutbox;
	std::vector<std::pair<bool, std::string>> localLogs;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_outbox.clear();

		sweepExpired();

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
				auto cancel = buildBeepCancel(i);
				if (cancel) _outbox.emplace_back(bd.addr, std::move(cancel));
				queueLog("Register beep: no answer from " + bd.ext + ", cancelled");
			}
			bd = BeepDialog{};   // free the slot
		}

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
			std::string caller = session->getSrc() ? session->getSrc()->getNumber() : "?";
			std::string callee = session->getDest() ? session->getDest()->getNumber() : "?";

			int durationSec = 0;
			if (session->getState() == Session::State::Connected)
			{
				durationSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
					now - session->getStartTime()).count());
			}
			nextSnapshot.sessions.emplace_back(caller, callee, sessionStateToString(session->getState()), durationSec);
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

		{
			std::lock_guard<std::mutex> snapLock(_snapshotMutex);
			_snapshot = std::move(nextSnapshot);
		}

		localOutbox = std::move(_outbox);
		_outbox.clear();

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
	for (auto& bd : _beepDialogs)
	{
		if (bd.state != BeepState::Free && bd.callID == callID)
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
	// ACK the phone's 200 OK to our beep INVITE (RFC 3261 §13.2.2.4 / §17.1.1.3).
	// Same Call-ID/branch/From-tag as the INVITE; To carries the phone's tag from the
	// 200. CSeq stays "1 ACK" (matches the INVITE transaction). No body.
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
	// branch, CSeq 2 BYE) within the established dialog. To carries the phone's tag.
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
	std::string branch = "z9hG4bK" + IDGen::GenerateID(12);

	std::ostringstream ss;
	ss << "BYE sip:" << bd->ext << "@" << destIpPort << " SIP/2.0\r\n"
	   << "Via: SIP/2.0/UDP " << srcIpPort << ";branch=" << branch << "\r\n"
	   << "From: \"PocketDial\" <sip:pbx@" << srcIpPort << ">;tag=" << bd->fromTag << "\r\n"
	   << "To: " << ok->getTo() << "\r\n"
	   << "Call-ID: " << bd->callID << "\r\n"
	   << "CSeq: 2 BYE\r\n"
	   << "Max-Forwards: 70\r\n"
	   << "Content-Length: 0\r\n\r\n";

	return getMessageFromPool(ss.str(), bd->addr);
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

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
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

	nvs_close(h);
#endif
}

void RequestsHandler::persistForwards()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	std::string blob;
	for (const auto& [ext, cfg] : _forwards)
	{
		blob += ext; blob += '\t';
		blob += cfg.always; blob += '\t';
		blob += cfg.busy; blob += '\t';
		blob += cfg.noAnswer; blob += '\n';
	}
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) == ESP_OK)
	{
		nvs_set_str(h, "forwards", blob.c_str());
		nvs_commit(h);
		nvs_close(h);
	}
#endif
}

void RequestsHandler::persistRingGroups()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	std::string blob;
	for (const auto& [ext, g] : _ringGroups)
	{
		blob += ext; blob += '\t';
		blob += (g.mode == pbx::GroupMode::Hunt ? "hunt" : "ringall"); blob += '\t';
		blob += pbx::joinMembers(g.members); blob += '\n';
	}
	nvs_handle_t h;
	if (nvs_open(NVS_PBX_NS, NVS_READWRITE, &h) == ESP_OK)
	{
		nvs_set_str(h, "groups", blob.c_str());
		nvs_commit(h);
		nvs_close(h);
	}
#endif
}

void RequestsHandler::loadCdrRing()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
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

void RequestsHandler::persistCdrRing()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
	// Serialize the ring oldest-first (same order loadCdrRing replays). Bounded by
	// POCKETDIAL_CDR_RECORDS, so the blob is fixed-footprint. Write-through on each
	// teardown: the CDR ring is small (default 32) and calls end infrequently
	// relative to flash endurance, so a per-call rewrite is acceptable — see summary.
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
	nvs_handle_t h;
	if (nvs_open(NVS_CDR_NS, NVS_READWRITE, &h) == ESP_OK)
	{
		nvs_set_str(h, "ring", blob.c_str());
		nvs_commit(h);
		nvs_close(h);
	}
#endif
}
