#include "LoopbackAnchorClient.hpp"
#include <chrono>
#include <atomic>

namespace
{
	std::atomic<uint64_t> g_activeCallId{0};
}

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
	g_activeCallId++;
	std::vector<std::thread> threadsToJoin;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		threadsToJoin = std::move(_simThreads);
	}
	for (auto& t : threadsToJoin)
	{
		if (t.joinable())
		{
			t.join();
		}
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
	uint64_t myCallId = ++g_activeCallId;
	_stopSimThread = false;

	{
		std::lock_guard<std::mutex> lock(_mutex);
		_simThreads.emplace_back([this, myCallId]() {
			// Simulate network delay for ringing
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			if (myCallId != g_activeCallId.load()) return;

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
			if (myCallId != g_activeCallId.load()) return;

			if (evCb)
			{
				CallEvent answerEv{CallEvent::Answered, _activeParticipantId, ""};
				evCb(answerEv);
			}
		});
	}

	return true;
}

bool LoopbackAnchorClient::dropCall(const std::string& participantId)
{
	if (!_connected)
	{
		return false;
	}

	std::string partId;
	EventCallback evCb;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		partId = participantId.empty() ? _activeParticipantId : participantId;
		if (partId.empty())
		{
			return false;
		}
		_activeParticipantId.clear();
		evCb = _eventCb;
	}

	_stopSimThread = true;
	g_activeCallId++;

	if (evCb)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_simThreads.emplace_back([evCb, partId]() {
			CallEvent dropEv{CallEvent::Dropped, partId, ""};
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
