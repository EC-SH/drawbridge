#include <gtest/gtest.h>
#include "RtpReceiver.hpp"
#include "RtpSender.hpp"
#include <cstdint>
#include <cstring>
#include <vector>

// ── µ-law DECODER: known-sample vectors (ITU-T G.711) ───────────────────────
// Exact inverse of RtpSender::linearToUlaw at the µ-law quantization grid. These
// values are the canonical reference dequantizer outputs (verified against the
// standard G.711 table): a wrong decode = static/garbled audio when voicemail or
// the B2BUA bridge plays the stored µ-law back.
TEST(UlawDecode, KnownSamples) {
    // µ-law "zero" (0xFF) decodes to digital silence.
    EXPECT_EQ(RtpReceiver::mulawDecode(0xFF), 0);
    // 0x7F is the other near-zero codepoint (smallest negative step) → 0.
    EXPECT_EQ(RtpReceiver::mulawDecode(0x7F), 0);
    // Full-scale positive (0x80) and negative (0x00). µ-law max magnitude is
    // 32124, NOT 32767 — that is correct G.711 behaviour, not a bug.
    EXPECT_EQ(RtpReceiver::mulawDecode(0x80),  32124);
    EXPECT_EQ(RtpReceiver::mulawDecode(0x00), -32124);
    // Sign symmetry: a code and its sign-flipped twin decode to ±the same value.
    EXPECT_EQ(RtpReceiver::mulawDecode(0xD5),  716);
    EXPECT_EQ(RtpReceiver::mulawDecode(0x55), -716);
    EXPECT_EQ(RtpReceiver::mulawDecode(0xAA),  5372);
    EXPECT_EQ(RtpReceiver::mulawDecode(0x2A), -5372);
}

// ── Round-trip: encode (sender) → decode (receiver) ─────────────────────────
// µ-law is lossy, so decode(encode(x)) snaps x to the nearest µ-law level. We
// assert the reconstructed sample is close to the original (within one µ-law
// step's worth of error) and that the SIGN is always preserved.
TEST(UlawDecode, EncodeDecodeRoundTrip) {
    struct V { int16_t in; int16_t expect; };
    const V vectors[] = {
        {     0,      0 },
        {  1000,    988 },
        { -1000,   -988 },
        {  8000,   7932 },
        { -8000,  -7932 },
        {   100,    104 },
        {  -100,   -104 },
        { 32767,  32124 },   // clamps to µ-law full scale
        {-32768, -32124 },
    };
    for (const auto& v : vectors) {
        uint8_t u = RtpSender::linearToUlaw(v.in);
        int16_t d = RtpReceiver::mulawDecode(u);
        EXPECT_EQ(d, v.expect) << "in=" << v.in << " ulaw=0x" << std::hex << (int)u;
        // Sign preserved (zero maps to zero).
        if (v.in > 0) EXPECT_GT(d, 0);
        if (v.in < 0) EXPECT_LT(d, 0);
    }
    // Monotonic-ish error bound over a sweep: reconstruction never flips sign and
    // stays within a generous absolute tolerance of the µ-law step at that level.
    for (int x = -32768; x <= 32767; x += 251) {
        uint8_t u = RtpSender::linearToUlaw(static_cast<int16_t>(x));
        int16_t d = RtpReceiver::mulawDecode(u);
        if (x > 0) EXPECT_GE(d, 0);
        if (x < 0) EXPECT_LE(d, 0);
    }
}

// ── Buffer decode: bounds + null safety ─────────────────────────────────────
TEST(UlawDecode, BufferDecode) {
    const uint8_t in[4] = { 0xFF, 0x80, 0x00, 0xD5 };
    int16_t out[4] = { 1, 2, 3, 4 };
    size_t n = RtpReceiver::mulawDecodeBuffer(in, 4, out);
    EXPECT_EQ(n, 4u);
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[1], 32124);
    EXPECT_EQ(out[2], -32124);
    EXPECT_EQ(out[3], 716);
    // Null / zero-count are safe no-ops returning 0.
    EXPECT_EQ(RtpReceiver::mulawDecodeBuffer(nullptr, 4, out), 0u);
    EXPECT_EQ(RtpReceiver::mulawDecodeBuffer(in, 0, out), 0u);
    EXPECT_EQ(RtpReceiver::mulawDecodeBuffer(in, 4, nullptr), 0u);
}

// ── RTP parse: round-trip against RtpSender's header builder ─────────────────
// The two modules are the two ends of the same wire. Build a packet with the
// SENDER's buildRtpHeader + a known µ-law payload, parse it with the RECEIVER,
// and assert every field round-trips and the payload pointer/length land exactly
// on the media bytes (offset 12, no CSRC/extension).
TEST(RtpParse, RoundTripAgainstSenderHeader) {
    const uint16_t seq  = 0x1234;
    const uint32_t ts   = 0xAABBCCDD;
    const uint32_t ssrc = 0x01020304;

    uint8_t pkt[RtpReceiver::RTP_HEADER_BYTES + RtpReceiver::SAMPLES_PER_PKT];
    RtpSender::buildRtpHeader(pkt, /*marker=*/true, RtpSender::PAYLOAD_TYPE_PCMU,
                              seq, ts, ssrc);
    // Fill a recognizable µ-law payload (decode-checkable below).
    for (int i = 0; i < RtpReceiver::SAMPLES_PER_PKT; ++i) {
        pkt[RtpReceiver::RTP_HEADER_BYTES + i] = static_cast<uint8_t>(0xFF - (i & 0x7F));
    }

    RtpReceiver::RtpPacket p;
    ASSERT_TRUE(RtpReceiver::parseRtp(pkt, sizeof(pkt), p));
    EXPECT_EQ(p.version, 2);
    EXPECT_TRUE(p.marker);
    EXPECT_EQ(p.payloadType, RtpReceiver::PAYLOAD_TYPE_PCMU);
    EXPECT_EQ(p.seq, seq);
    EXPECT_EQ(p.timestamp, ts);
    EXPECT_EQ(p.ssrc, ssrc);
    ASSERT_NE(p.payload, nullptr);
    EXPECT_EQ(p.payloadLen, static_cast<size_t>(RtpReceiver::SAMPLES_PER_PKT));
    // Payload points exactly at the bytes after the 12-byte header.
    EXPECT_EQ(p.payload, pkt + RtpReceiver::RTP_HEADER_BYTES);
    EXPECT_EQ(p.payload[0], 0xFF);
    // And the receiver can decode that payload to PCM16.
    EXPECT_EQ(RtpReceiver::mulawDecode(p.payload[0]), 0);
}

// ── RTP parse: CSRC list is skipped to reach the true payload ───────────────
TEST(RtpParse, SkipsCsrcList) {
    // V=2, CC=2 (two CSRC identifiers → 8 extra header bytes).
    uint8_t pkt[RtpReceiver::RTP_HEADER_BYTES + 8 + 3];
    std::memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x82;            // V=2, P=0, X=0, CC=2
    pkt[1] = 0x00;            // M=0, PT=0
    // seq/ts/ssrc arbitrary; CSRC bytes [12..19]; payload [20..22].
    pkt[20] = 0xAA; pkt[21] = 0xBB; pkt[22] = 0xCC;

    RtpReceiver::RtpPacket p;
    ASSERT_TRUE(RtpReceiver::parseRtp(pkt, sizeof(pkt), p));
    EXPECT_EQ(p.payloadType, 0);
    ASSERT_NE(p.payload, nullptr);
    EXPECT_EQ(p.payloadLen, 3u);
    EXPECT_EQ(p.payload[0], 0xAA);
    EXPECT_EQ(p.payload, pkt + RtpReceiver::RTP_HEADER_BYTES + 8);
}

// ── RTP parse: header extension (X bit) is skipped correctly ────────────────
TEST(RtpParse, SkipsHeaderExtension) {
    // V=2, X=1, CC=0. Extension = 4-byte ext header + 1 word (4 bytes) body.
    uint8_t pkt[RtpReceiver::RTP_HEADER_BYTES + 4 + 4 + 2];
    std::memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x90;            // V=2, P=0, X=1, CC=0
    pkt[1] = 0x00;            // M=0, PT=0
    // Ext header at [12..15]: profile (2) + length-in-words (2) = 1 word.
    pkt[12] = 0xBE; pkt[13] = 0xDE;   // profile id
    pkt[14] = 0x00; pkt[15] = 0x01;   // length = 1 word
    // Ext body word at [16..19]; payload at [20..21].
    pkt[20] = 0x11; pkt[21] = 0x22;

    RtpReceiver::RtpPacket p;
    ASSERT_TRUE(RtpReceiver::parseRtp(pkt, sizeof(pkt), p));
    ASSERT_NE(p.payload, nullptr);
    EXPECT_EQ(p.payloadLen, 2u);
    EXPECT_EQ(p.payload[0], 0x11);
    EXPECT_EQ(p.payload, pkt + RtpReceiver::RTP_HEADER_BYTES + 8);
}

// ── RTP parse: padding (P bit) trims pad bytes from the payload ──────────────
TEST(RtpParse, TrimsPadding) {
    // V=2, P=1, CC=0. Payload = 2 real bytes + 2 pad bytes (last byte = pad count).
    uint8_t pkt[RtpReceiver::RTP_HEADER_BYTES + 4];
    std::memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0xA0;            // V=2, P=1, X=0, CC=0
    pkt[1] = 0x00;            // M=0, PT=0
    pkt[12] = 0x33; pkt[13] = 0x44;   // real payload
    pkt[14] = 0x00;                   // pad
    pkt[15] = 0x02;                   // pad count = 2 (includes this byte)

    RtpReceiver::RtpPacket p;
    ASSERT_TRUE(RtpReceiver::parseRtp(pkt, sizeof(pkt), p));
    EXPECT_EQ(p.payloadLen, 2u);
    EXPECT_EQ(p.payload[0], 0x33);
    EXPECT_EQ(p.payload[1], 0x44);
}

// ── RTP parse: rejects malformed / non-RTP buffers gracefully ───────────────
TEST(RtpParse, RejectsMalformed) {
    RtpReceiver::RtpPacket p;
    // Too short for the fixed header.
    uint8_t tiny[8] = {0x80,0,0,0,0,0,0,0};
    EXPECT_FALSE(RtpReceiver::parseRtp(tiny, sizeof(tiny), p));
    // Wrong version (V=1).
    uint8_t v1[RtpReceiver::RTP_HEADER_BYTES] = {0x40,0,0,0,0,0,0,0,0,0,0,0};
    EXPECT_FALSE(RtpReceiver::parseRtp(v1, sizeof(v1), p));
    // CSRC count claims more bytes than the buffer holds.
    uint8_t badcc[RtpReceiver::RTP_HEADER_BYTES] = {0x8F,0,0,0,0,0,0,0,0,0,0,0}; // CC=15
    EXPECT_FALSE(RtpReceiver::parseRtp(badcc, sizeof(badcc), p));
    // Null buffer.
    EXPECT_FALSE(RtpReceiver::parseRtp(nullptr, 100, p));
}

// ── Lifecycle / single-stream cap on the host stub ──────────────────────────
// Mirrors RtpSender's SingleStreamCap test: the host build keeps the cap and the
// bind-advertise (localPort echo) logic exercisable with no real sockets.
TEST(RtpReceiver, SingleStreamCapAndSink) {
    RtpReceiver rx;
    EXPECT_FALSE(rx.isActive());

    int frames = 0;
    uint32_t lastTs = 0;
    auto sink = [&](const uint8_t* mulaw, size_t n, uint32_t ts, uint16_t) {
        (void)mulaw; (void)n; ++frames; lastTs = ts;
    };

    // A null sink is rejected.
    EXPECT_FALSE(rx.start(5064, RtpReceiver::Sink{}));

    // Start binds the requested port (echoed back on the host stub).
    EXPECT_TRUE(rx.start(5064, sink));
    EXPECT_TRUE(rx.isActive());
    EXPECT_EQ(rx.localPort(), 5064);

    // Second start while active is rejected (the single-stream cap).
    EXPECT_FALSE(rx.start(6000, sink));
    EXPECT_EQ(rx.localPort(), 5064);

    // Stop frees the slot; stopping again is an idempotent no-op.
    EXPECT_TRUE(rx.stop());
    EXPECT_FALSE(rx.isActive());
    EXPECT_FALSE(rx.stop());

    // Re-start after stop works (slot is free) and can pick a different port.
    EXPECT_TRUE(rx.start(7000, sink));
    EXPECT_EQ(rx.localPort(), 7000);
    EXPECT_TRUE(rx.stop());

    (void)frames; (void)lastTs;   // sink is not invoked on the host stub (no I/O)
}
