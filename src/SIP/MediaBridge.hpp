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
