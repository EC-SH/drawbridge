#include "MediaBridge.hpp"

MediaBridge::MediaBridge() = default;

MediaBridge::~MediaBridge()
{
	stopBridge();
}

void MediaBridge::init(RtpReceiver* receiver, RtpSender* sender, AnchorClient* anchor)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_receiver = receiver;
	_sender   = sender;
	_anchor   = anchor;
}

bool MediaBridge::startBridge(const std::string& handsetIp, uint16_t handsetPort, const std::string& callID)
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (_active.load(std::memory_order_acquire))
	{
		return false; // single active bridge allowed
	}

	if (!_receiver || !_sender || !_anchor)
	{
		return false;
	}

	_playoutBuffer.clear();
	_callID = callID;
	_active.store(true, std::memory_order_release);

	// Register audio rx callback with the WAN anchor client (inbound HTTP GET stream)
	_anchor->registerAudioRxCallback([this](const int16_t* samples, size_t count) {
		if (_active.load(std::memory_order_acquire))
		{
			_playoutBuffer.write(samples, count);
		}
	});

	// Start the LAN RTP receiver (dynamic ephemeral port)
	bool receiverStarted = _receiver->start(0, [this](const uint8_t* mulaw, size_t n, uint32_t /*timestamp*/, uint16_t /*seq*/) {
		if (!_active.load(std::memory_order_acquire))
		{
			return;
		}

		// Decode incoming LAN handset µ-law audio to PCM16
		int16_t decoded[320];
		size_t toDecode = (n > 320) ? 320 : n;
		size_t decodedCount = RtpReceiver::mulawDecodeBuffer(mulaw, toDecode, decoded);

		// Write the PCM16 samples to the WAN anchor POST stream
		if (decodedCount > 0 && _anchor)
		{
			_anchor->writeAudio(decoded, decodedCount);
		}
	});

	if (!receiverStarted)
	{
		_anchor->registerAudioRxCallback(nullptr);
		_callID.clear();
		_active.store(false, std::memory_order_release);
		return false;
	}

	// Start the LAN RTP sender to stream to the handset
	bool senderStarted = _sender->start(handsetIp, handsetPort, callID, [this](uint8_t* outUlaw, size_t count) {
		if (!_active.load(std::memory_order_acquire))
		{
			return false;
		}

		// Read linear PCM16 samples from the playout buffer
		int16_t pcmSamples[320];
		size_t toRead = (count > 320) ? 320 : count;

		bool readOk = _playoutBuffer.read(pcmSamples, toRead);

		// Encode linear PCM16 samples to µ-law
		for (size_t i = 0; i < toRead; ++i)
		{
			outUlaw[i] = RtpSender::linearToUlaw(pcmSamples[i]);
		}

		return readOk; // Returns false if underrun occurred, but outUlaw is still filled with comfort noise
	});

	if (!senderStarted)
	{
		_receiver->stop();
		_anchor->registerAudioRxCallback(nullptr);
		_callID.clear();
		_active.store(false, std::memory_order_release);
		return false;
	}

	return true;
}

void MediaBridge::stopBridge()
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (!_active.load(std::memory_order_acquire))
	{
		return;
	}

	_active.store(false, std::memory_order_release);

	if (_receiver)
	{
		_receiver->stop();
	}
	if (_sender)
	{
		_sender->stop(_callID);
	}
	if (_anchor)
	{
		_anchor->registerAudioRxCallback(nullptr);
	}

	_playoutBuffer.clear();
	_callID.clear();
}
