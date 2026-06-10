#ifndef LOOPBACK_ANCHOR_CLIENT_HPP
#define LOOPBACK_ANCHOR_CLIENT_HPP

#include "AnchorClient.hpp"
#include <mutex>
#include <thread>
#include <atomic>
#include <string>

class LoopbackAnchorClient : public AnchorClient
{
public:
	LoopbackAnchorClient();
	~LoopbackAnchorClient() override;

	bool init(const std::string& baseUrl,
	          const std::string& clientId,
	          const std::string& clientSecret,
	          const std::string& sourceDn) override;

	bool start() override;
	void stop() override;
	bool isConnected() const override;
	bool makeCall(const std::string& destination) override;
	bool dropCall(const std::string& participantId) override;
	void setEventCallback(EventCallback cb) override;
	bool writeAudio(const int16_t* pcmSamples, size_t count) override;
	void registerAudioRxCallback(AudioRxCallback cb) override;

private:
	std::string _baseUrl;
	std::string _clientId;
	std::string _sourceDn;

	std::atomic<bool> _connected{false};
	std::string _activeParticipantId;

	EventCallback   _eventCb;
	AudioRxCallback _audioCb;

	mutable std::mutex _mutex;

	std::thread       _simThread;
	std::atomic<bool> _stopSimThread{false};
};

#endif // LOOPBACK_ANCHOR_CLIENT_HPP
