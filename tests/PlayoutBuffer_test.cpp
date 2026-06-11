#include <gtest/gtest.h>
#include "PlayoutBuffer.hpp"
#include "LoopbackAnchorClient.hpp"
#include <vector>
#include <chrono>

TEST(PlayoutBufferTest, BasicWriteRead)
{
	PlayoutBuffer buffer(100);
	EXPECT_EQ(buffer.getLength(), 0);

	int16_t inputData[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
	size_t written = buffer.write(inputData, 10);
	EXPECT_EQ(written, 10);
	EXPECT_EQ(buffer.getLength(), 10);

	int16_t outputData[10] = { 0 };
	bool readOk = buffer.read(outputData, 10);
	EXPECT_TRUE(readOk);
	EXPECT_EQ(buffer.getLength(), 0);

	for (int i = 0; i < 10; ++i)
	{
		EXPECT_EQ(outputData[i], inputData[i]);
	}
}

TEST(PlayoutBufferTest, UnderrunComfortNoise)
{
	PlayoutBuffer buffer(100);
	EXPECT_EQ(buffer.getLength(), 0);

	int16_t outputData[20] = { 0 };
	bool readOk = buffer.read(outputData, 20);

	// Read should report false (underrun)
	EXPECT_FALSE(readOk);
	EXPECT_EQ(buffer.getUnderruns(), 1);

	// Buffer should remain empty
	EXPECT_EQ(buffer.getLength(), 0);

	// Output data should be filled with comfort noise within [-20, 20] range
	for (int i = 0; i < 20; ++i)
	{
		EXPECT_GE(outputData[i], -20);
		EXPECT_LE(outputData[i], 20);
	}
}

TEST(PlayoutBufferTest, OverrunOldestFrameDrop)
{
	// Small buffer capacity of 10 samples
	PlayoutBuffer buffer(10);

	int16_t firstBatch[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	buffer.write(firstBatch, 8);
	EXPECT_EQ(buffer.getLength(), 8);

	// Writing 5 more samples (total 13) exceeds capacity (10), triggering overrun
	int16_t secondBatch[5] = { 9, 10, 11, 12, 13 };
	size_t written = buffer.write(secondBatch, 5);

	EXPECT_EQ(written, 5);
	EXPECT_EQ(buffer.getLength(), 10); // clamped to max capacity
	EXPECT_EQ(buffer.getOverruns(), 1);

	// The oldest 3 samples (1, 2, 3) should have been dropped.
	// Remaining samples in buffer should be: 4, 5, 6, 7, 8, 9, 10, 11, 12, 13.
	int16_t outputData[10] = { 0 };
	bool readOk = buffer.read(outputData, 10);
	EXPECT_TRUE(readOk);

	int16_t expected[10] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
	for (int i = 0; i < 10; ++i)
	{
		EXPECT_EQ(outputData[i], expected[i]);
	}
}

TEST(LoopbackAnchorClientTest, SimEventsAndAudioLoop)
{
	LoopbackAnchorClient client;
	EXPECT_FALSE(client.isConnected());

	// Init and start
	client.init("https://localhost", "app-id", "app-secret", "999");
	EXPECT_TRUE(client.start());
	EXPECT_TRUE(client.isConnected());

	// Set event callback
	std::atomic<int> ringingCount{0};
	std::atomic<int> answerCount{0};
	std::atomic<int> droppedCount{0};

	client.setEventCallback([&](const AnchorClient::CallEvent& ev) {
		if (ev.type == AnchorClient::CallEvent::Ringing)
		{
			ringingCount++;
		}
		else if (ev.type == AnchorClient::CallEvent::Answered)
		{
			answerCount++;
		}
		else if (ev.type == AnchorClient::CallEvent::Dropped)
		{
			droppedCount++;
		}
	});

	// Trigger simulated makeCall
	EXPECT_TRUE(client.makeCall("102"));

	// Sleep to let ringing and answer events fire
	std::this_thread::sleep_for(std::chrono::milliseconds(60));

	EXPECT_EQ(ringingCount, 1);
	EXPECT_EQ(answerCount, 1);

	// Verify audio loopback
	std::atomic<bool> audioReceived{false};
	int16_t txAudio[10] = { 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000 };

	client.registerAudioRxCallback([&](const int16_t* samples, size_t count) {
		EXPECT_EQ(count, 10);
		for (size_t i = 0; i < count; ++i)
		{
			EXPECT_EQ(samples[i], txAudio[i]);
		}
		audioReceived = true;
	});

	EXPECT_TRUE(client.writeAudio(txAudio, 10));
	EXPECT_TRUE(audioReceived);

	// Tear down call — Dropped event fires async; stop() joins the sim thread
	// so the assertion is placed after stop() to guarantee the callback ran.
	EXPECT_TRUE(client.dropCall("mock-part-123"));
	client.stop();
	EXPECT_EQ(droppedCount, 1);
	EXPECT_FALSE(client.isConnected());
}

// Inbound (Mode 1) anchor contract: an upstream-delivered PSTN call surfaces as a
// single CallEvent::Incoming carrying the participant id + caller id; answering it
// (answerCall, the mirror of makeCall) drives the participant to Answered — exactly
// the sequence the engine's routeInboundAnchorCall → onInboundAnchorOk path drives.
TEST(LoopbackAnchorClientTest, InboundCallAnnounceAndAnswer)
{
	LoopbackAnchorClient client;
	client.init("https://localhost", "app-id", "app-secret", "113");
	EXPECT_TRUE(client.start());

	std::atomic<int> incomingCount{0};
	std::atomic<int> answerCount{0};
	std::string seenParticipant;
	std::string seenCaller;

	client.setEventCallback([&](const AnchorClient::CallEvent& ev) {
		if (ev.type == AnchorClient::CallEvent::Incoming)
		{
			incomingCount++;
			seenParticipant = ev.participantId;
			seenCaller = ev.callerId;
		}
		else if (ev.type == AnchorClient::CallEvent::Answered)
		{
			answerCount++;
		}
	});

	// Upstream offers a PSTN call to the monitored DN.
	client.simulateInboundCall("+15551234567");
	std::this_thread::sleep_for(std::chrono::milliseconds(40));
	EXPECT_EQ(incomingCount, 1);
	EXPECT_FALSE(seenParticipant.empty());
	EXPECT_EQ(seenCaller, "+15551234567");

	// The engine answers once the local extension picks up → leg connects (Answered).
	EXPECT_TRUE(client.answerCall(seenParticipant));
	std::this_thread::sleep_for(std::chrono::milliseconds(40));
	EXPECT_EQ(answerCount, 1);

	client.stop();
	EXPECT_FALSE(client.isConnected());
}
