#ifndef RTP_RECEIVER_HPP
#define RTP_RECEIVER_HPP

// RtpReceiver — the inbound media path, mirror image of RtpSender.
//
// Where RtpSender stands up a one-way RTP *source* (synthesize tone → µ-law
// encode → RTP packetize → sendto), RtpReceiver stands up the inverse one-way
// RTP *sink*: bind a UDP socket, recvfrom RTP datagrams, parse the RFC 3550
// header, validate the payload type (PCMU/µ-law only), and hand the µ-law
// payload to a caller-supplied sink. It is foundational infrastructure that
// three future consumers all need but none of them lives here:
//   (1) Voicemail record — write the raw µ-law frames to the `prompts` flash
//       partition (esp_partition_write, fixed-frame-size raw layout).
//   (2) SBC trunk B2BUA bridge — hand each frame to the other call leg's
//       RtpSender-style egress so audio is relayed between two dialogs.
//   (3) Call recording — tee the µ-law (or decoded PCM16) to storage.
// RtpReceiver deliberately knows NONE of that: the `Sink` callback is the only
// coupling point, so this module stays a clean standalone class. Integration
// into RequestsHandler is a separate, later step.
//
// Layering / portability (identical to RtpSender):
//   * The pure parse + DSP helpers (RTP header parse, µ-law DECODE) are
//     PLATFORM-INDEPENDENT and host-unit-tested (see tests/RtpReceiver_test.cpp).
//   * The actual UDP socket + the receive FreeRTOS task are ESP-ONLY and guarded
//     by `#if defined(ESP_PLATFORM)`. On host they compile to no-op stubs so the
//     desktop gtest build still builds & links and start()/stop()/isActive()
//     remain exercisable.
//
// Concurrency cap: ONE concurrent stream (matches RtpSender). A second start()
// while a stream is live returns false. isActive() is the single source of truth
// for the cap, updated under an internal guard so the SIP thread and the receive
// task never race the slot.
//
// No hot-path heap: each datagram is read into a fixed member/stack buffer and
// decoded into a fixed buffer; nothing is allocated per packet. The Sink is
// invoked with pointers into that fixed buffer (the sink must consume/copy
// synchronously — it must not retain the pointer past the callback).

#if defined(ESP_PLATFORM) || defined(ESP32)
#include <lwip/sockets.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#elif defined(__linux__)
#include <netinet/in.h>
#elif defined _WIN32 || defined _WIN64
#include <WinSock2.h>
#endif

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

class RtpReceiver
{
public:
	// G.711 @ 8 kHz, 20 ms ptime → 160 samples / packet. Payload type 0 (PCMU).
	// These MUST stay in lock-step with RtpSender's matching constants: the two
	// modules are the two ends of the same wire format.
	static constexpr int     SAMPLE_RATE_HZ    = 8000;
	static constexpr int     PTIME_MS          = 20;
	static constexpr int     SAMPLES_PER_PKT   = SAMPLE_RATE_HZ * PTIME_MS / 1000; // 160
	static constexpr uint8_t PAYLOAD_TYPE_PCMU = 0;
	static constexpr int     RTP_HEADER_BYTES  = 12;

	// Largest RTP datagram we are willing to buffer. A PCMU/20ms packet is
	// 12 + 160 = 172 bytes; we allow generous slack for CSRC lists / a header
	// extension / non-20ms ptime without ever heap-allocating. Anything larger
	// is read but truncated by the kernel to this cap; parseRtp() then bounds-
	// checks against the actual byte count so a short/oversize read is safe.
	static constexpr int     MAX_DATAGRAM_BYTES = 512;

	// Parsed view of one RTP packet (RFC 3550 §5.1). `payload`/`payloadLen`
	// point INTO the caller's receive buffer — no copy, no allocation. Valid
	// only while that buffer is alive.
	struct RtpPacket
	{
		uint8_t        version     = 0;
		bool           marker      = false;
		uint8_t        payloadType = 0;
		uint16_t       seq         = 0;
		uint32_t       timestamp   = 0;
		uint32_t       ssrc        = 0;
		const uint8_t* payload     = nullptr;  // → into the source datagram buffer
		size_t         payloadLen  = 0;
	};

	// The Sink: callers choose what to do with each received audio frame without
	// RtpReceiver knowing. Invoked once per accepted (PT==0) packet, ON THE
	// RECEIVE TASK, with:
	//   mulaw      : pointer to the raw G.711 µ-law payload bytes (NOT decoded —
	//                voicemail/relay consumers want µ-law as-is on the wire).
	//   n          : number of µ-law bytes (== decoded PCM16 sample count).
	//   timestamp  : the packet's RTP timestamp (8 kHz units) for jitter/ordering.
	//   seq        : the packet's RTP sequence number (for gap detection upstream).
	// CONTRACT: the sink MUST consume synchronously and MUST NOT retain `mulaw`
	// past return (the buffer is reused for the next datagram). To get PCM16, the
	// sink calls RtpReceiver::mulawDecodeBuffer() into its own storage.
	using Sink = std::function<void(const uint8_t* mulaw, size_t n,
		uint32_t timestamp, uint16_t seq)>;

	// ── Pure, platform-independent primitives (host-unit-tested) ────────────────

	// ITU-T G.711 µ-law DECODE of one companded byte → 16-bit linear PCM. Exact
	// inverse of RtpSender::linearToUlaw at the µ-law quantization granularity
	// (µ-law is lossy, so decode(encode(x)) snaps x to the nearest µ-law level).
	static int16_t mulawDecode(uint8_t ulaw);

	// Decode `count` µ-law bytes from `in` into `out` (PCM16). `out` must hold
	// `count` int16_t. No allocation, fully bounds-driven by `count`. Safe no-op
	// on null/empty. Returns the number of samples written.
	static size_t mulawDecodeBuffer(const uint8_t* in, size_t count, int16_t* out);

	// Parse an RTP packet (RFC 3550 §5.1) out of `data`/`len` into `out`.
	// Correctly skips the CSRC list (CC * 4 bytes) and a present header extension
	// (X bit: a 4-byte ext header + a length-prefixed body) so `payload` points at
	// the true media start. Returns false (and leaves `out` untouched) if the
	// buffer is too short, the version is not 2, or the computed payload offset
	// runs past `len`. Used by the receive task AND the host tests.
	static bool parseRtp(const uint8_t* data, size_t len, RtpPacket& out);

	// ── Stream lifecycle (the registrar will drive these from the SIP thread) ───

	RtpReceiver();
	~RtpReceiver();

	// True while a stream is live (socket bound + receive task running). The
	// registrar checks this to enforce the single-stream cap.
	bool isActive() const { return _active.load(std::memory_order_acquire); }

	// The UDP port the receiver is bound on (0 means not started). Lets the
	// caller advertise it in an SDP answer.
	int localPort() const { return _localPort.load(std::memory_order_acquire); }

	// Bind `localPort` and start the receive task, delivering accepted PCMU
	// frames to `sink`. Pass localPort 0 to let the OS pick an ephemeral port
	// (then read it back via localPort()). Returns false if a stream is already
	// active (cap reached) or the socket/task could not be created. On host this
	// is a guarded no-op that still flips _active and records the sink/port so the
	// cap + bind-advertise logic is exercisable in tests.
	bool start(uint16_t localPort, Sink sink);

	// Stop the stream: close the socket (unblocks recvfrom), signal the receive
	// task to exit, clear _active and the sink. Idempotent — safe on an already-
	// idle receiver. Returns true if a stream was actually stopped.
	bool stop();

private:
	// Common, platform-independent slot reset used by stop()/teardown. Caller must
	// hold _slotMutex. Clears the sink and active/port state.
	void clearSlotLocked();

#if defined(ESP_PLATFORM) || defined(ESP32)
	static void taskTrampoline(void* arg);
	void runLoop();                       // recvfrom-paced receive loop (Core 0)

	int               _sock = -1;
	// Cross-thread control flags — same ownership model as RtpSender:
	//   _stopRequested : the SIP thread asks the receive task to exit. Closing the
	//                    socket also unblocks a parked recvfrom() immediately.
	//   _taskRunning   : true from just-before-launch until the task's last act.
	//                    The task OWNS its teardown (closes its own socket, clears
	//                    the slot) and clears _taskRunning LAST, so the destructor
	//                    can join on it and start() refuses to overlap a dying task.
	std::atomic<bool> _stopRequested{false};
	std::atomic<bool> _taskRunning{false};
#endif

	std::atomic<bool> _active{false};
	std::atomic<int>  _localPort{0};      // bound RX port (0 = not started)

	// Guards the sink + the start/stop transition so the SIP thread and the
	// receive task never race the slot. Non-recursive (matches RtpSender).
	mutable std::mutex _slotMutex;
	Sink               _sink;             // owner of the live stream's consumer
};

#endif
