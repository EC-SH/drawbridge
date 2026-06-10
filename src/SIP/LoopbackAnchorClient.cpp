#include "LoopbackAnchorClient.hpp"
#include <chrono>

LoopbackAnchorClient::LoopbackAnchorClient() = default;

LoopbackAnchorClient::~LoopbackAnchorClient()
{
	stop();
}

bool LoopbackAnchorClient::init(const std::string& baseUrl,
                               const std::string& clientId,
                               const std::string& /*clientSecret*/,
                               const std::string& sourceDn)
{
	_baseUrl = baseUrl;
	_clientId = clientId;
	_sourceDn = sourceDn;
	return true;
}

bool LoopbackAnchorClient::start()
{
	_connected = true;
	return true;
}

void LoopbackAnchorClient::stop()
{
	_connected = false;
	_stopSimThread = true;
	if (_simThread.joinable())
	{
		_simThread.join();
	}
	std::lock_guard<std::mutex> lock(_mutex);
	_eventCb = nullptr;
	_audioCb = nullptr;
}

bool LoopbackAnchorClient::isConnected() const
{
	return _connected;
}

bool LoopbackAnchorClient::makeCall(const std::string& /*destination*/)
{
	if (!_connected)
	{
		return false;
	}

	_stopSimThread = true;
	if (_simThread.joinable())
	{
		_simThread.join();
	}
	_stopSimThread = false;

	_simThread = std::thread([this]() {
		// Simulate network delay for ringing
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		if (_stopSimThread) return;

		EventCallback evCb;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			evCb = _eventCb;
			_activeParticipantId = "mock-part-123";
		}

		if (evCb)
		{
			CallEvent ringingEv{CallEvent::Ringing, _activeParticipantId, ""};
			evCb(ringingEv);
		}

		// Simulate call answering
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		if (_stopSimThread) return;

		if (evCb)
		{
			CallEvent answerEv{CallEvent::Answered, _activeParticipantId, ""};
			evCb(answerEv);
		}
	});

	return true;
}

bool LoopbackAnchorClient::dropCall(const std::string& participantId)
{
	_stopSimThread = true;
	if (_simThread.joinable())
	{
		_simThread.join();
	}

	EventCallback evCb;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		evCb = _eventCb;
		_activeParticipantId.clear();
	}

	if (evCb)
	{
		// Fire asynchronously: the caller (e.g. onBye) may hold RequestsHandler::_mutex,
		// and the event callback also takes that mutex — synchronous dispatch deadlocks.
		_simThread = std::thread([evCb, participantId]() {
			CallEvent dropEv{CallEvent::Dropped, participantId, ""};
			evCb(dropEv);
		});
	}

	return true;
}

void LoopbackAnchorClient::setEventCallback(EventCallback cb)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_eventCb = cb;
}

bool LoopbackAnchorClient::writeAudio(const int16_t* pcmSamples, size_t count)
{
	if (!_connected)
	{
		return false;
	}

	AudioRxCallback audioCb;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		audioCb = _audioCb;
	}

	if (audioCb && pcmSamples != nullptr && count > 0)
	{
		audioCb(pcmSamples, count);
	}

	return true;
}

void LoopbackAnchorClient::registerAudioRxCallback(AudioRxCallback cb)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_audioCb = cb;
}
