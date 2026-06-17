#ifndef MEDIA_BRIDGE_HPP
#define MEDIA_BRIDGE_HPP

#include "RtpReceiver.hpp"
#include "RtpSender.hpp"
#include "AnchorClient.hpp"
#include "PlayoutBuffer.hpp"
#include <string>
#include <atomic>
#include <mutex>

class MediaBridge
{
public:
	MediaBridge();
	~MediaBridge();

	// Set up the bridge dependencies
	void init(RtpReceiver* receiver, RtpSender* sender, AnchorClient* anchor);

	// Start bridging a handset's RTP stream to the media anchor. participantId is the upstream
	// anchor participant this bridge serves — it tags writeAudio() so the anchor routes this
	// bridge's handset audio to the right POST stream when N calls run concurrently (#100).
	bool startBridge(const std::string& handsetIp, uint16_t handsetPort, const std::string& callID, const std::string& participantId);

	// Stop all active streams and tear down the bridge
	void stopBridge();

	// Check if the bridge is currently active
	bool isActive() const { return _active.load(std::memory_order_acquire); }

	// #100: route one inbound (3CX->device) PCM chunk to this bridge's playout buffer IFF this
	// bridge is active and serving `participantId`. Returns true if it consumed the chunk. The
	// anchor exposes a SINGLE rx callback; RequestsHandler owns it and fans out to the bridge that
	// owns the participant (the bridge no longer registers the anchor callback itself).
	bool feedRx(const std::string& participantId, const int16_t* samples, size_t count);

	// #100: lookups so RequestsHandler can find the bridge serving a participant / call id (for
	// per-call teardown + the rx fan-out). Both return false when the bridge is idle.
	bool isFor(const std::string& participantId) const;
	bool isForCallId(const std::string& callID) const;
	// The participant / call id this bridge is serving ("" if idle). For the orphan-bridge sweep:
	// drop the upstream leg of a bridge that has outlived its session.
	std::string participantId() const;
	std::string callId() const;

	// The UDP port the bridge's RTP receiver is bound on (0 if not active). This
	// is the port the handset must send its audio to, so it MUST be the value
	// advertised in the 200 OK SDP answer — not the sender's source port.
	int receiverPort() const;

	// Access statistics
	PlayoutBuffer& getPlayoutBuffer() { return _playoutBuffer; }

private:
	// Pointers to the shared dependencies
	RtpReceiver* _receiver = nullptr;
	RtpSender*   _sender = nullptr;
	AnchorClient* _anchor = nullptr;

	PlayoutBuffer _playoutBuffer;

	std::atomic<bool> _active{false};
	std::string       _callID;
	std::string       _participantId;   // #100: the upstream anchor participant id this bridge serves

	mutable std::mutex _mutex;
};

#endif // MEDIA_BRIDGE_HPP
