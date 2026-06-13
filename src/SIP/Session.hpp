#ifndef SESSION_HPP
#define SESSION_HPP

// Session.hpp: Issue #28 resolved.
#include <memory>
#include <chrono>
#include <vector>

class SipMessage;
#include "SipClient.hpp"

class Session
{
public:

	enum class State
	{
		Invited,
		Busy,
		Unavailable,
		Cancel,
		Bye,
		Connected,
	};


	Session();
	Session(std::string callID, std::shared_ptr<SipClient> src);

	void reset(std::string callID, std::shared_ptr<SipClient> src);

	void setState(State state);
	void setDest(std::shared_ptr<SipClient> dest);

	const std::string& getCallID() const;
	std::shared_ptr<SipClient> getSrc() const;
	std::shared_ptr<SipClient> getDest() const;
	State getState() const;
	std::chrono::steady_clock::time_point getStartTime() const;

	// Broadcast / Forking helpers
	bool isBroadcast() const { return _isBroadcast; }
	void setBroadcast(bool val) { _isBroadcast = val; }

	// True only for sessions bridged to the WAN media anchor (3CX/loopback).
	// The anchor event callbacks and CANCEL/BYE teardown branches MUST match on
	// this — not on "dest is a non-pool client", which is also true of the 777
	// echo, 440 tone, and *69/*11 virtual sessions and made anchor teardown hit
	// whichever virtual session happened to be first in the map.
	bool isAnchor() const { return _isAnchor; }
	void setAnchor(bool val) { _isAnchor = val; }

	// Inbound anchor calls invert the SIP roles of an outbound one: the server is the
	// UAC that originated the INVITE *toward the handset* (dest), so teardown/ACK must
	// address the handset and carry our From-tag. These fields hold the extra dialog
	// state an inbound leg needs that an outbound (server-as-UAS) leg does not:
	//   _remoteTag           — the handset's To-tag, learned from its 200 OK
	//   _uacBranch           — the Via branch of our INVITE (reused for the CANCEL)
	//   _anchorParticipantId — the upstream participant id to answerCall()/dropCall()
	bool isAnchorInbound() const { return _anchorInbound; }
	void setAnchorInbound(bool val) { _anchorInbound = val; }
	const std::string& getRemoteTag() const { return _remoteTag; }
	void setRemoteTag(const std::string& tag) { _remoteTag = tag; }
	const std::string& getUacBranch() const { return _uacBranch; }
	void setUacBranch(const std::string& branch) { _uacBranch = branch; }
	const std::string& getAnchorParticipantId() const { return _anchorParticipantId; }
	void setAnchorParticipantId(const std::string& id) { _anchorParticipantId = id; }

	const std::vector<std::shared_ptr<SipClient>>& getPendingTargets() const { return _pendingTargets; }
	void setPendingTargets(std::vector<std::shared_ptr<SipClient>> targets) { _pendingTargets = std::move(targets); }
	void removePendingTarget(const std::string& number);

	std::shared_ptr<SipMessage> getInviteMessage() const { return _inviteMessage; }
	void setInviteMessage(std::shared_ptr<SipMessage> msg) { _inviteMessage = msg; }

	// The To-tag this UAS generated for the dialog. RFC 3261: the tag is created
	// once (on the first dialog-establishing response, our 180 Ringing) and MUST be
	// reused unchanged on the 200 OK. A fresh tag on the 200 makes it a different
	// dialog, which Yealink-class phones never reconcile — they keep ringing.
	const std::string& getLocalTag() const { return _localTag; }
	void setLocalTag(const std::string& tag) { _localTag = tag; }

	// ── No-answer / hunt-group timer (Class A sweep) ──────────────────
	// A non-default _ringDeadline arms a one-shot timer that tick() polls: on
	// expiry the registrar CANCELs the outstanding leg and advances (CFNA forward,
	// or the next hunt member). hasRingTimer()/isRingExpired() are pure queries;
	// clearRingTimer() disarms it once the call connects or is torn down.
	void armRingTimer(std::chrono::steady_clock::time_point deadline) { _ringDeadline = deadline; _ringTimerArmed = true; }
	void clearRingTimer() { _ringTimerArmed = false; }
	bool hasRingTimer() const { return _ringTimerArmed; }
	bool isRingExpired(std::chrono::steady_clock::time_point now) const { return _ringTimerArmed && now >= _ringDeadline; }

	// CFNA forward target: where to send the call when the no-answer timer fires.
	const std::string& getNoAnswerTarget() const { return _noAnswerTarget; }
	void setNoAnswerTarget(const std::string& t) { _noAnswerTarget = t; }

	// ── Sequential hunt-group progression (Class A sweep) ─────────────
	// _huntMembers is the ordered list; _huntIndex is the member currently ringing.
	// _huntActive distinguishes a hunt session from an ordinary/broadcast one.
	bool isHunt() const { return _huntActive; }
	void setHunt(bool v) { _huntActive = v; }
	std::vector<std::string>& getHuntMembers() { return _huntMembers; }
	void setHuntMembers(std::vector<std::string> m) { _huntMembers = std::move(m); }
	size_t getHuntIndex() const { return _huntIndex; }
	void setHuntIndex(size_t i) { _huntIndex = i; }

	// ── Call parking (park-orbit bridge) ──────────────────────────────
	// peerCallID links the two dialog legs of a retrieved (or rung-back) parked
	// call so a BYE from either side relays a server BYE to the other leg.
	// parkUac marks the leg the SERVER originated (the park-timeout ring-back
	// INVITE toward the parker), which inverts the From/To roles of that relay.
	const std::string& getPeerCallID() const { return _peerCallID; }
	void setPeerCallID(const std::string& id) { _peerCallID = id; }
	bool isParkUac() const { return _parkUac; }
	void setParkUac(bool v) { _parkUac = v; }

	// The group extension this fork is servicing (for CDR / logging), if any.
	const std::string& getGroupExt() const { return _groupExt; }
	void setGroupExt(const std::string& g) { _groupExt = g; }

	void release();

private:
	std::string _callID;
	std::shared_ptr<SipClient> _src;
	std::shared_ptr<SipClient> _dest;
	State _state;
	std::chrono::steady_clock::time_point _startTime;

	bool _isBroadcast = false;
	bool _isAnchor = false;
	bool _anchorInbound = false;
	std::string _remoteTag;            // handset To-tag (inbound anchor leg)
	std::string _uacBranch;            // our INVITE Via branch (inbound anchor leg)
	std::string _anchorParticipantId;  // upstream participant id (inbound anchor leg)
	std::vector<std::shared_ptr<SipClient>> _pendingTargets;
	std::shared_ptr<SipMessage> _inviteMessage;
	std::string _localTag; // UAS-generated To-tag, shared across 180 + 200 OK

	// No-answer / hunt timer + forwarding/hunt bookkeeping (Class A sweep).
	std::chrono::steady_clock::time_point _ringDeadline;
	bool _ringTimerArmed = false;
	std::string _noAnswerTarget;

	bool _huntActive = false;
	std::vector<std::string> _huntMembers;
	size_t _huntIndex = 0;

	std::string _groupExt;

	// Call parking (park-orbit bridge): linked peer leg + server-as-UAC marker.
	std::string _peerCallID;
	bool _parkUac = false;
};

#endif
