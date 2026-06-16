#ifndef ANCHOR_CLIENT_HPP
#define ANCHOR_CLIENT_HPP

#include <string>
#include <functional>
#include <cstdint>
#include <cstddef>

class AnchorClient
{
public:
	struct CallEvent
	{
		// Ringing/Answered/Dropped/Dtmf describe a call the device ORIGINATED (outbound,
		// via makeCall). Incoming is the inverse: the upstream is delivering a PSTN call
		// to a DN this device monitors, and the device must ring a local extension and
		// then answerCall() to take it. callerId carries the PSTN caller's number/name
		// when the upstream provides it (best-effort; may be empty).
		enum Type { Ringing, Answered, Dropped, Dtmf, Incoming } type;
		std::string participantId;
		std::string dtmfDigit;
		std::string callerId;
	};
	using EventCallback = std::function<void(const CallEvent&)>;
	using AudioRxCallback = std::function<void(const std::string& participantId, const int16_t* pcmSamples, size_t count)>;

	virtual ~AnchorClient() = default;

	// Initialize credentials and endpoints
	virtual bool init(const std::string& baseUrl,
	                  const std::string& clientId,
	                  const std::string& clientSecret,
	                  const std::string& sourceDn) = 0;

	// Connect control WebSocket and OAuth flow
	virtual bool start() = 0;

	// Disconnect and release all handles
	virtual void stop() = 0;

	// Status query
	virtual bool isConnected() const = 0;

	// Originate a PSTN call
	virtual bool makeCall(const std::string& destination) = 0;

	// Answer an inbound participant the upstream is offering to a monitored DN (the
	// mirror of makeCall). Called once the local extension has accepted, so the
	// upstream connects the PSTN leg and the media streams can open. participantId is
	// the id carried by the CallEvent::Incoming that announced the call.
	virtual bool answerCall(const std::string& participantId) = 0;

	// Hang up an active call
	virtual bool dropCall(const std::string& participantId) = 0;

	// Register listener for control channel events
	virtual void setEventCallback(EventCallback cb) = 0;

	// Stream audio chunks to a specific call's POST stream, keyed by participant id so N
	// concurrent anchor calls each route to their own upstream stream (#100).
	virtual bool writeAudio(const std::string& participantId, const int16_t* pcmSamples, size_t count) = 0;

	// Register the callback that receives audio chunks from the anchor's GET stream(s). The
	// callback is invoked with the participant id of the call the audio belongs to, so the
	// consumer routes each call's inbound audio to its own MediaBridge (#100).
	virtual void registerAudioRxCallback(AudioRxCallback cb) = 0;

	// Periodic, non-blocking maintenance pump, driven from RequestsHandler::tick()
	// (≤1 Hz). Implementations must do only constant-time work here — read flags,
	// spawn a worker for any blocking I/O — never block, log, or allocate. The
	// ThreeCx anchor uses it to run the _outboundActive reconcile watchdog.
	virtual void tick() = 0;

	// TLS handshake telemetry: how many media-stream opens did a FULL handshake (cold ECDHE)
	// vs RESUMED a cached session (the fast path). Lets /api/status show whether resumption is
	// holding and empirically reveals the session-ticket lifetime (full count climbs after an
	// idle gap when the ticket expired). Default 0/0 for anchors that don't track it (loopback).
	virtual void getTlsHandshakeStats(uint32_t& fullOut, uint32_t& resumedOut) const
	{
		fullOut = 0;
		resumedOut = 0;
	}

	// #107: operator-configurable cadence (SECONDS) for the idle TLS re-warm heartbeat.
	// The anchor periodically refreshes its media-stream TLS session WHILE IDLE so even the
	// first call after a long quiet stretch resumes its session instead of paying a cold
	// handshake at connect. 0 disables it. This is a SETTING, not a constant, so a different
	// upstream provider's session-ticket lifetime (or a change in the current one) is a
	// config edit rather than a recompile. Default no-op for anchors that don't stream media
	// (loopback); the real implementation is in ThreeCxAnchorClient.
	virtual void setRewarmIntervalSec(uint32_t sec) { (void)sec; }
};

#endif // ANCHOR_CLIENT_HPP
