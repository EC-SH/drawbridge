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

	const std::vector<std::shared_ptr<SipClient>>& getPendingTargets() const { return _pendingTargets; }
	void setPendingTargets(std::vector<std::shared_ptr<SipClient>> targets) { _pendingTargets = std::move(targets); }
	void removePendingTarget(const std::string& number);

	std::shared_ptr<SipMessage> getInviteMessage() const { return _inviteMessage; }
	void setInviteMessage(std::shared_ptr<SipMessage> msg) { _inviteMessage = msg; }

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
	std::vector<std::shared_ptr<SipClient>> _pendingTargets;
	std::shared_ptr<SipMessage> _inviteMessage;

	// No-answer / hunt timer + forwarding/hunt bookkeeping (Class A sweep).
	std::chrono::steady_clock::time_point _ringDeadline;
	bool _ringTimerArmed = false;
	std::string _noAnswerTarget;

	bool _huntActive = false;
	std::vector<std::string> _huntMembers;
	size_t _huntIndex = 0;

	std::string _groupExt;
};

#endif
