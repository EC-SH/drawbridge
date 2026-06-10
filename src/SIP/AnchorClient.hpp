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
		enum Type { Ringing, Answered, Dropped, Dtmf } type;
		std::string participantId;
		std::string dtmfDigit;
	};
	using EventCallback = std::function<void(const CallEvent&)>;
	using AudioRxCallback = std::function<void(const int16_t* pcmSamples, size_t count)>;

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

	// Hang up an active call
	virtual bool dropCall(const std::string& participantId) = 0;

	// Register listener for control channel events
	virtual void setEventCallback(EventCallback cb) = 0;

	// Stream audio chunks to the active call's POST stream
	virtual bool writeAudio(const int16_t* pcmSamples, size_t count) = 0;

	// Register callback to receive audio chunks from the active call's GET stream
	virtual void registerAudioRxCallback(AudioRxCallback cb) = 0;
};

#endif // ANCHOR_CLIENT_HPP
