#ifndef PLAYOUT_BUFFER_HPP
#define PLAYOUT_BUFFER_HPP

#include <vector>
#include <mutex>
#include <cstdint>
#include <cstddef>
#include <atomic>

class PlayoutBuffer
{
public:
	// Default max samples is 8000 (1 second @ 8 kHz)
	explicit PlayoutBuffer(size_t maxSamples = 8000);
	~PlayoutBuffer() = default;

	// Write linear PCM16 samples to the buffer (called by the HTTP RX thread).
	// If the buffer overflows, oldest samples are dropped to make room.
	// Returns the number of samples successfully written.
	size_t write(const int16_t* samples, size_t count);

	// Read linear PCM16 samples from the buffer (called by the RTP TX thread every 20ms).
	// Always fills 'out' with 'count' samples. If underrun, fills the rest with
	// low-amplitude comfort noise.
	// Returns true if all requested samples were read from the buffer,
	// false if an underrun occurred.
	bool read(int16_t* out, size_t count);

	// Get current buffered sample count
	size_t getLength() const;

	// Reset buffer (clear all samples, reset stats)
	void clear();

	// Statistics queries
	uint64_t getUnderruns() const { return _underruns.load(std::memory_order_relaxed); }
	uint64_t getOverruns() const { return _overruns.load(std::memory_order_relaxed); }
	size_t getTargetDepth() const { return _targetDepth.load(std::memory_order_relaxed); }

	// Set/Get target playout delay (in samples)
	void setTargetDepth(size_t samples);

private:
	std::vector<int16_t> _buffer;
	size_t _maxSamples;
	size_t _readPtr = 0;
	size_t _writePtr = 0;
	size_t _count = 0;

	mutable std::mutex _mutex;

	// Target depth for playout (e.g. 80ms = 640 samples @ 8 kHz)
	std::atomic<size_t> _targetDepth{640};

	// Statistics
	std::atomic<uint64_t> _underruns{0};
	std::atomic<uint64_t> _overruns{0};
};

#endif // PLAYOUT_BUFFER_HPP
