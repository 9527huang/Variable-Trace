#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BorrowedTransport.hpp"
#include "RecorderHandler.hpp"
#include "RecorderLayout.hpp"
#include "SerialDebugProbe.hpp"
#include "SerialTargetSimulator.hpp"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/spdlog.h"

/*
 * The recorder is the one part of this build whose numbers have to match another
 * program exactly: the offsets of the fields, the order samples are packed in,
 * and the arithmetic that turns a circular buffer into a time series. Getting any
 * of them wrong produces plausible numbers rather than an error, so the checks
 * below compare against the firmware's own declarations instead of against a
 * second table written from the same understanding.
 *
 * The round trip at the end drives a real probe and a real handler against a
 * stand in for the target, so what is checked there is the whole path: frames on
 * the wire, writes landing at the right offsets, and the samples that come back.
 */
namespace
{
	recorder::Settings makeSettings(uint32_t version, uint32_t revision, uint32_t timestepNs,
									uint32_t maxBufferSize, uint32_t maxVariables, uint16_t floatSupport)
	{
		recorder::Settings settings;
		settings.version = version;
		settings.revision = revision;
		settings.timestepNs = timestepNs;
		settings.maxBufferSize = maxBufferSize;
		settings.maxVariables = maxVariables;
		settings.floatSupport = floatSupport;
		return settings;
	}

	/* The structures the firmware declares, written out here so that the offsets
	   the host computes can be checked against something a compiler laid out. */
	namespace reference
	{
		struct Variable
		{
			uint32_t address;
			uint32_t size;
		};

		template <uint32_t BufferSize>
		struct Buffer
		{
			uint32_t head;
			uint32_t start;
			uint8_t data[BufferSize];
		};

		template <bool FloatSupport>
		struct Trigger;

		template <>
		struct Trigger<true>
		{
			uint8_t state;
			uint8_t edge;
			uint8_t type;
			uint32_t sourceAddress;
			uint32_t preTriggerSamples;
			int32_t triggerValue;
			int32_t actValue;
			int32_t prevValue;
			uint32_t triggerValueU32;
			uint32_t actValueU32;
			uint32_t prevValueU32;
			float triggerValueFloat;
			float actValueFloat;
			float prevValueFloat;
		};

		template <>
		struct Trigger<false>
		{
			uint8_t state;
			uint8_t edge;
			uint8_t type;
			uint32_t sourceAddress;
			uint32_t preTriggerSamples;
			int32_t triggerValue;
			int32_t actValue;
			int32_t prevValue;
			uint32_t triggerValueU32;
			uint32_t actValueU32;
			uint32_t prevValueU32;
		};

		template <uint32_t MaxVariables, uint32_t BufferSize, bool FloatSupport>
		struct Recorder
		{
			uint32_t samplesToSkip;
			uint32_t variableCount;
			uint32_t maxActualBufferSize;
			uint16_t samplesPackSize;
			uint32_t samplesSinceStart;
			uint32_t skippedSamples;
			uint8_t state;
			Variable sampleList[MaxVariables];
			Buffer<BufferSize> recorderbuf;
			Trigger<FloatSupport> trigger;
		};

		template <uint32_t MaxVariables, uint32_t BufferSize, bool FloatSupport>
		using R = Recorder<MaxVariables, BufferSize, FloatSupport>;
	}  // namespace reference

	void expectLayoutMatches(const recorder::Settings& settings, uint32_t elementSize)
	{
		const recorder::Layout layout = recorder::makeLayout(settings, elementSize);

		if (settings.maxVariables == 12 && settings.maxBufferSize == 32768 && settings.floatSupport != 0)
		{
			using R = reference::R<12, 32768, true>;
			using B = reference::Buffer<32768>;
			using T = reference::Trigger<true>;

			EXPECT_EQ(layout.samplesToSkipOffset, offsetof(R, samplesToSkip));
			EXPECT_EQ(layout.variableCountOffset, offsetof(R, variableCount));
			EXPECT_EQ(layout.maxActualBufferSizeOffset, offsetof(R, maxActualBufferSize));
			EXPECT_EQ(layout.samplesPackSizeOffset, offsetof(R, samplesPackSize));
			EXPECT_EQ(layout.samplesSinceStartOffset, offsetof(R, samplesSinceStart));
			EXPECT_EQ(layout.skippedSamplesOffset, offsetof(R, skippedSamples));
			EXPECT_EQ(layout.stateOffset, offsetof(R, state));
			EXPECT_EQ(layout.sampleListOffset, offsetof(R, sampleList));
			EXPECT_EQ(layout.bufferHeadOffset, offsetof(R, recorderbuf) + offsetof(B, head));
			EXPECT_EQ(layout.bufferStartOffset, offsetof(R, recorderbuf) + offsetof(B, start));
			EXPECT_EQ(layout.bufferDataOffset, offsetof(R, recorderbuf) + offsetof(B, data));
			EXPECT_EQ(layout.bufferByteSize, offsetof(R, trigger) - offsetof(R, recorderbuf) - offsetof(B, data));
			EXPECT_EQ(layout.triggerOffset, offsetof(R, trigger));
			EXPECT_EQ(layout.triggerStateOffset, offsetof(R, trigger) + offsetof(T, state));
			EXPECT_EQ(layout.triggerEdgeOffset, offsetof(R, trigger) + offsetof(T, edge));
			EXPECT_EQ(layout.triggerTypeOffset, offsetof(R, trigger) + offsetof(T, type));
			EXPECT_EQ(layout.triggerSourceAddressOffset, offsetof(R, trigger) + offsetof(T, sourceAddress));
			EXPECT_EQ(layout.triggerPreTriggerSamplesOffset, offsetof(R, trigger) + offsetof(T, preTriggerSamples));
			EXPECT_EQ(layout.triggerValueOffset, offsetof(R, trigger) + offsetof(T, triggerValue));
			EXPECT_EQ(layout.triggerActValueOffset, offsetof(R, trigger) + offsetof(T, actValue));
			EXPECT_EQ(layout.triggerPrevValueOffset, offsetof(R, trigger) + offsetof(T, prevValue));
			EXPECT_EQ(layout.triggerValueU32Offset, offsetof(R, trigger) + offsetof(T, triggerValueU32));
			EXPECT_EQ(layout.triggerActValueU32Offset, offsetof(R, trigger) + offsetof(T, actValueU32));
			EXPECT_EQ(layout.triggerPrevValueU32Offset, offsetof(R, trigger) + offsetof(T, prevValueU32));
			EXPECT_EQ(layout.triggerValueFloatOffset, offsetof(R, trigger) + offsetof(T, triggerValueFloat));
			EXPECT_EQ(layout.triggerActValueFloatOffset, offsetof(R, trigger) + offsetof(T, actValueFloat));
			EXPECT_EQ(layout.triggerPrevValueFloatOffset, offsetof(R, trigger) + offsetof(T, prevValueFloat));
			EXPECT_EQ(layout.totalSize, sizeof(R));
			return;
		}

		if (settings.maxVariables == 4 && settings.maxBufferSize == 64 && settings.floatSupport == 0)
		{
			using R = reference::R<4, 64, false>;
			using B = reference::Buffer<64>;
			using T = reference::Trigger<false>;

			EXPECT_EQ(layout.sampleListOffset, offsetof(R, sampleList));
			EXPECT_EQ(layout.bufferHeadOffset, offsetof(R, recorderbuf) + offsetof(B, head));
			EXPECT_EQ(layout.bufferDataOffset, offsetof(R, recorderbuf) + offsetof(B, data));
			EXPECT_EQ(layout.triggerOffset, offsetof(R, trigger));
			EXPECT_EQ(layout.triggerPrevValueU32Offset, offsetof(R, trigger) + offsetof(T, prevValueU32));
			/* Without float support there are no float members, and the offset
			   lands just past the struct. The host reads the flag from the
			   settings struct before it writes there. */
			EXPECT_EQ(layout.triggerValueFloatOffset, offsetof(R, trigger) + sizeof(T));
			EXPECT_EQ(layout.totalSize, sizeof(R));
			return;
		}

		FAIL() << "no reference structure was declared for this configuration";
	}

	/* A logger that keeps the tests quiet without changing what is exercised. */
	std::shared_ptr<spdlog::logger> quietLogger()
	{
		auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
		return std::make_shared<spdlog::logger>("recorder test", sink);
	}
}  // namespace

TEST(RecorderLayoutTest, MatchesTheStructuresTheFirmwareDeclares)
{
	expectLayoutMatches(makeSettings(8, 3, 10000, 32768, 12, 1), 1);
	expectLayoutMatches(makeSettings(8, 3, 10000, 64, 4, 0), 1);
}

TEST(RecorderLayoutTest, LeavesRoomForTheWholeRecorder)
{
	const recorder::Settings settings = makeSettings(8, 3, 10000, 32768, 12, 1);
	const recorder::Layout layout = recorder::makeLayout(settings);

	EXPECT_EQ(layout.bufferByteSize, 32768u);
	EXPECT_EQ(layout.totalSize, 28u + 12u * 8u + 8u + 32768u + 48u);
}

TEST(RecorderLayoutTest, CountsTwoBytesPerElementOnAC2000)
{
	const recorder::Settings settings = makeSettings(8, 3, 10000, 1024, 8, 1);
	const recorder::Layout layout = recorder::makeLayout(settings, 2);

	EXPECT_EQ(layout.bufferByteSize, 2048u);
	EXPECT_EQ(layout.triggerOffset, 28u + 8u * 8u + 8u + 2048u);
}

TEST(RecorderSettingsTest, ReadsTheStructTheTargetPublishes)
{
	/* The initialiser of ____recorderSettings in recorder.c, as bytes. */
	const uint8_t raw[] = {
		8,    0, 0, 0,      // version
		3,    0, 0, 0,      // revision
		0x10, 0x27, 0, 0,   // timestepNs = 10000
		0,    0x80, 0, 0,   // maxBufferSize = 32768
		12,   0, 0, 0,      // maxVariables
		1,    0,            // floatSupport
		0,    0,            // padding
	};

	const recorder::Settings settings = recorder::parseSettings(raw, sizeof(raw));

	EXPECT_EQ(settings.version, 8u);
	EXPECT_EQ(settings.revision, 3u);
	EXPECT_EQ(settings.timestepNs, 10000u);
	EXPECT_EQ(settings.maxBufferSize, 32768u);
	EXPECT_EQ(settings.maxVariables, 12u);
	EXPECT_EQ(settings.floatSupport, 1);
}

TEST(RecorderSettingsTest, ASHortBufferIsNotEnough)
{
	const uint8_t raw[10] = {};
	EXPECT_EQ(recorder::parseSettings(raw, sizeof(raw)).maxBufferSize, 0u);
	EXPECT_EQ(recorder::parseSettings(nullptr, 0).version, 0u);
}

TEST(RecorderRingTest, AnUntouchedBufferHoldsNothing)
{
	recorder::RingState ring;
	ring.maxActualBufferSize = 1024;
	ring.packSize = 8;
	ring.state = recorder::State::Running;
	ring.head = 0;
	ring.start = 0;
	ring.samplesSinceStart = 0;

	const recorder::RingWindow window = recorder::ringWindowOf(ring);

	EXPECT_EQ(window.bytes, 0u);
	EXPECT_TRUE(recorder::validRanges(window, ring.maxActualBufferSize).empty());
}

TEST(RecorderRingTest, APartlyFilledBufferStartsAtTheBeginning)
{
	recorder::RingState ring;
	ring.maxActualBufferSize = 1024;
	ring.packSize = 8;
	ring.state = recorder::State::Running;
	ring.head = 40;
	ring.start = 0;
	ring.samplesSinceStart = 5;

	const recorder::RingWindow window = recorder::ringWindowOf(ring);

	EXPECT_EQ(window.bytes, 40u);
	EXPECT_EQ(window.oldest, 0u);
}

TEST(RecorderRingTest, AFullBufferIsToldApartFromAnEmptyOneByTheSampleCounter)
{
	/* Both of these leave the write pointer at zero, and they mean opposite
	   things. The sample counter is the only thing that separates them. */
	recorder::RingState justStarted;
	justStarted.maxActualBufferSize = 1024;
	justStarted.packSize = 8;
	justStarted.state = recorder::State::Running;
	justStarted.samplesSinceStart = 0;

	recorder::RingState wrapped;
	wrapped = justStarted;
	wrapped.samplesSinceStart = 128;

	EXPECT_EQ(recorder::ringWindowOf(justStarted).bytes, 0u);

	const recorder::RingWindow window = recorder::ringWindowOf(wrapped);
	EXPECT_EQ(window.bytes, 1024u);
	EXPECT_EQ(window.oldest, 0u);
}

TEST(RecorderRingTest, AWrappedBufferStartsWhereTheWritePointerStanding)
{
	recorder::RingState ring;
	ring.maxActualBufferSize = 1024;
	ring.packSize = 8;
	ring.state = recorder::State::Running;
	ring.head = 16;
	ring.start = 0;
	ring.samplesSinceStart = 200;

	const recorder::RingWindow window = recorder::ringWindowOf(ring);

	EXPECT_EQ(window.bytes, 1024u);
	EXPECT_EQ(window.oldest, 16u);
}

TEST(RecorderRingTest, ATriggeredBufferStartsAtTheTriggerBoundary)
{
	recorder::RingState ring;
	ring.maxActualBufferSize = 1024;
	ring.packSize = 8;
	ring.state = recorder::State::Running;
	ring.head = 600;
	ring.start = 200;

	const recorder::RingWindow window = recorder::ringWindowOf(ring);

	EXPECT_EQ(window.oldest, 200u);
	EXPECT_EQ(window.bytes, 400u);
}

TEST(RecorderRingTest, AFinishedSweepFillsTheWholeBuffer)
{
	recorder::RingState ring;
	ring.maxActualBufferSize = 1024;
	ring.packSize = 8;
	ring.state = recorder::State::Full;
	ring.head = 0;
	ring.start = 200;

	const recorder::RingWindow window = recorder::ringWindowOf(ring);

	EXPECT_EQ(window.bytes, 1024u);
	EXPECT_EQ(window.oldest, 200u);
}

TEST(RecorderRingTest, OnlyWholeSamplesAreReported)
{
	recorder::RingState ring;
	ring.maxActualBufferSize = 1024;
	ring.packSize = 6;
	ring.state = recorder::State::Running;
	ring.head = 0;
	ring.start = 0;
	ring.samplesSinceStart = 100;

	const recorder::RingWindow window = recorder::ringWindowOf(ring);

	/* A hundred samples of six bytes do not fit in a thousand and twenty four
	   bytes, and the part that sticks out is not a sample. */
	EXPECT_EQ(window.bytes, 600u);
}

TEST(RecorderRingTest, TheWindowIsOneRangeUntilItWraps)
{
	const std::vector<std::pair<uint32_t, uint32_t>> flat = recorder::validRanges({100, 400}, 1024);

	ASSERT_EQ(flat.size(), 1u);
	EXPECT_EQ(flat[0].first, 100u);
	EXPECT_EQ(flat[0].second, 400u);

	const std::vector<std::pair<uint32_t, uint32_t>> wrapped = recorder::validRanges({900, 400}, 1024);

	ASSERT_EQ(wrapped.size(), 2u);
	EXPECT_EQ(wrapped[0].first, 900u);
	EXPECT_EQ(wrapped[0].second, 124u);
	EXPECT_EQ(wrapped[1].first, 0u);
	EXPECT_EQ(wrapped[1].second, 276u);
}

TEST(RecorderDecodeTest, TurnsThePackedBytesBackIntoVariables)
{
	recorder::RingState ring;
	ring.state = recorder::State::Running;
	ring.maxActualBufferSize = 64;
	ring.packSize = 6;
	ring.head = 18;
	ring.start = 0;
	ring.samplesSinceStart = 3;

	const std::vector<recorder::Slot> variables = {
		{0x20000000, 4, static_cast<uint8_t>(recorder::TriggerType::I32), "counter"},
		{0x20000004, 2, static_cast<uint8_t>(recorder::TriggerType::U16), "level"},
	};

	std::vector<uint8_t> buffer;

	for (uint32_t sample = 0; sample < 3; sample++)
	{
		const uint32_t counter = 0x1000 + sample;
		const uint16_t level = static_cast<uint16_t>(0x20 + sample);

		buffer.push_back(static_cast<uint8_t>(counter & 0xFF));
		buffer.push_back(static_cast<uint8_t>((counter >> 8) & 0xFF));
		buffer.push_back(static_cast<uint8_t>((counter >> 16) & 0xFF));
		buffer.push_back(static_cast<uint8_t>((counter >> 24) & 0xFF));
		buffer.push_back(static_cast<uint8_t>(level & 0xFF));
		buffer.push_back(static_cast<uint8_t>(level >> 8));
	}

	recorder::Capture capture;
	std::string error;

	ASSERT_TRUE(recorder::decodeRing(ring, variables, buffer, 10000, 0, capture, error)) << error;

	ASSERT_EQ(capture.samples, 3u);
	ASSERT_EQ(capture.series.size(), 2u);
	EXPECT_EQ(capture.series[0][0], 0x1000u);
	EXPECT_EQ(capture.series[0][2], 0x1002u);
	EXPECT_EQ(capture.series[1][0], 0x20u);
	EXPECT_EQ(capture.series[1][2], 0x22u);
	EXPECT_NEAR(capture.samplePeriodSeconds, 1e-5, 1e-12);
	EXPECT_NEAR(capture.timestamps[2], 2e-5, 1e-12);
}

TEST(RecorderDecodeTest, DownsamplingStretchesTheSamplePeriod)
{
	recorder::RingState ring;
	ring.state = recorder::State::Running;
	ring.maxActualBufferSize = 64;
	ring.packSize = 4;
	ring.head = 8;
	ring.start = 0;
	ring.samplesSinceStart = 2;

	const std::vector<recorder::Slot> variables = {{0x20000000, 4, static_cast<uint8_t>(recorder::TriggerType::U32), "v"}};
	const std::vector<uint8_t> buffer(8, 0);

	recorder::Capture capture;
	std::string error;

	ASSERT_TRUE(recorder::decodeRing(ring, variables, buffer, 10000, 9, capture, error)) << error;

	/* Ten steps of the timer interrupt between two stored samples. */
	EXPECT_NEAR(capture.samplePeriodSeconds, 1e-4, 1e-12);
}

TEST(RecorderDecodeTest, RefusesAGroupThatDoesNotAddUpToTheSample)
{
	recorder::RingState ring;
	ring.state = recorder::State::Running;
	ring.maxActualBufferSize = 64;
	ring.packSize = 6;
	ring.head = 6;
	ring.samplesSinceStart = 1;

	const std::vector<recorder::Slot> variables = {{0x20000000, 4, static_cast<uint8_t>(recorder::TriggerType::I32), "v"}};

	recorder::Capture capture;
	std::string error;

	EXPECT_FALSE(recorder::decodeRing(ring, variables, std::vector<uint8_t>(6, 0), 10000, 0, capture, error));
	EXPECT_FALSE(error.empty());
}

TEST(RecorderDecodeTest, RefusesFewerBytesThanTheWindowNeeds)
{
	recorder::RingState ring;
	ring.state = recorder::State::Running;
	ring.maxActualBufferSize = 64;
	ring.packSize = 4;
	ring.head = 16;
	ring.samplesSinceStart = 4;

	const std::vector<recorder::Slot> variables = {{0x20000000, 4, static_cast<uint8_t>(recorder::TriggerType::U32), "v"}};

	recorder::Capture capture;
	std::string error;

	EXPECT_FALSE(recorder::decodeRing(ring, variables, std::vector<uint8_t>(8, 0), 10000, 0, capture, error));
}

/* ---------------------------------------------------------------------------
 * Round trip
 * ------------------------------------------------------------------------ */

namespace
{
	constexpr uint32_t settingsAddress = 0x20000100;
	constexpr uint32_t recorderAddress = 0x20001000;

	constexpr uint32_t counterAddress = 0x20002000;
	constexpr uint32_t levelAddress = 0x20002004;

	recorder::Settings standardSettings()
	{
		return makeSettings(8, 3, 10000, 32768, 12, 1);
	}

	std::vector<recorder::Slot> standardVariables()
	{
		return {
			{counterAddress, 4, static_cast<uint8_t>(recorder::TriggerType::I32), "counter"},
			{levelAddress, 2, static_cast<uint8_t>(recorder::TriggerType::U16), "level"},
		};
	}

	/* Everything a round trip needs, so each test only writes down what it is
	   actually about. */
	class RecorderRoundTrip : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			logger = quietLogger();
			simulator = std::make_unique<RecorderSimulator>(target, settingsAddress, recorderAddress, standardSettings());
			probe = std::make_unique<SerialDebugProbe>(logger.get(), std::make_unique<BorrowedTransport>(target));
			handler = std::make_unique<recorder::RecorderHandler>(logger.get());
			handler->setSymbols(settingsAddress, recorderAddress);

			IDebugProbe::DebugProbeSettings probeSettings;
			probeSettings.debugProbe = IDebugProbe::Serial;
			probeSettings.serialNumber = "SIM";
			probeSettings.baudrate = 115200;

			std::vector<std::pair<uint32_t, uint8_t>> sampleList = {{counterAddress, 4}, {levelAddress, 2}};

			ASSERT_TRUE(probe->startAcqusition(probeSettings, sampleList, 100));
		}

		void feed(uint32_t index)
		{
			target.poke32(counterAddress, index);
			target.poke16(levelAddress, static_cast<uint16_t>(index * 7));
		}

		std::shared_ptr<spdlog::logger> logger;
		SerialTargetSimulator target;
		std::unique_ptr<RecorderSimulator> simulator;
		std::unique_ptr<SerialDebugProbe> probe;
		std::unique_ptr<recorder::RecorderHandler> handler;
		std::string error;
	};
}  // namespace

TEST_F(RecorderRoundTrip, DetectsTheTargetThroughTheLine)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	EXPECT_TRUE(handler->isDetected());
	EXPECT_EQ(handler->getDetectedSettings().version, 8u);
	EXPECT_EQ(handler->getDetectedSettings().revision, 3u);
	EXPECT_EQ(handler->getDetectedSettings().maxVariables, 12u);
	EXPECT_EQ(handler->getDetectedSettings().maxBufferSize, 32768u);
	EXPECT_EQ(handler->getLayout().totalSize, 28u + 96u + 8u + 32768u + 48u);
}

TEST_F(RecorderRoundTrip, RefusesATargetThatIsNotARecorder)
{
	/* A version the host does not know could have moved a field, and a wrong
	   offset produces numbers rather than an error. */
	RecorderSimulator other(target, settingsAddress, recorderAddress, makeSettings(7, 3, 10000, 32768, 12, 1));

	EXPECT_FALSE(handler->detect(*probe, error));
	EXPECT_NE(error.find("recorder 7.3"), std::string::npos);
}

TEST_F(RecorderRoundTrip, NeedsTheSymbolsBeforeItCanDetect)
{
	handler->clearSymbols();

	EXPECT_FALSE(handler->detect(*probe, error));
	EXPECT_NE(error.find("____recorder"), std::string::npos);
}

TEST_F(RecorderRoundTrip, WritesTheConfigurationIntoTheTarget)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	const std::vector<recorder::Slot> variables = standardVariables();

	recorder::Config config;
	config.mode = recorder::Mode::Run;
	config.variables = variables;

	ASSERT_TRUE(handler->apply(*probe, config, error)) << error;

	EXPECT_EQ(simulator->variableCount(), 2u);
	EXPECT_EQ(simulator->packSize(), 6);
	/* Whole samples only, so that the target never writes past its own ring. */
	EXPECT_EQ(simulator->maxActualBufferSize(), (32768u / 6u) * 6u);
	EXPECT_EQ(simulator->variableAddress(0), counterAddress);
	EXPECT_EQ(simulator->variableSize(0), 4u);
	EXPECT_EQ(simulator->variableAddress(1), levelAddress);
	EXPECT_EQ(simulator->variableSize(1), 2u);
	EXPECT_EQ(simulator->state(), recorder::State::Init);
	EXPECT_EQ(simulator->samplesToSkip(), 0u);

	/* Run mode keeps the trigger ready but unarmed, which is what makes the
	   buffer wrap instead of stopping. */
	EXPECT_EQ(simulator->triggerState(), recorder::TriggerState::Ready);
	EXPECT_EQ(simulator->triggerType(), recorder::TriggerType::Unknown);
}

TEST_F(RecorderRoundTrip, WithoutATriggerTheOtherModesFillTheBufferOnce)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	recorder::Config config;
	config.mode = recorder::Mode::Single;
	config.variables = standardVariables();

	ASSERT_TRUE(handler->apply(*probe, config, error)) << error;

	EXPECT_EQ(simulator->triggerState(), recorder::TriggerState::Setup);
	EXPECT_EQ(simulator->triggerType(), recorder::TriggerType::Unknown);
}

TEST_F(RecorderRoundTrip, BringsTheSamplesBack)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	const std::vector<recorder::Slot> variables = standardVariables();

	recorder::Config config;
	config.mode = recorder::Mode::Run;
	config.variables = variables;

	ASSERT_TRUE(handler->start(*probe, config, error)) << error;
	EXPECT_EQ(simulator->state(), recorder::State::Running);

	for (uint32_t index = 0; index < 200; index++)
	{
		feed(index);
		simulator->step();
	}

	recorder::Capture capture;
	ASSERT_TRUE(handler->download(*probe, variables, capture, error)) << error;

	ASSERT_EQ(capture.samples, 200u);
	ASSERT_EQ(capture.series.size(), 2u);
	EXPECT_EQ(capture.series[0][0], 0u);
	EXPECT_EQ(capture.series[0][10], 10u);
	EXPECT_EQ(capture.series[0][199], 199u);
	EXPECT_EQ(capture.series[1][10], 70u);
	EXPECT_NEAR(capture.samplePeriodSeconds, 1e-5, 1e-12);
	EXPECT_NEAR(capture.timestamps[199], 199e-5, 1e-12);
	EXPECT_EQ(handler->getLastDownloadSize(), 200u * 6u);
}

TEST_F(RecorderRoundTrip, KeepsOnlyTheNewestSamplesOnceTheBufferWraps)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	const std::vector<recorder::Slot> variables = standardVariables();

	recorder::Config config;
	config.mode = recorder::Mode::Run;
	config.variables = variables;

	ASSERT_TRUE(handler->start(*probe, config, error)) << error;

	const uint32_t capacity = (32768u / 6u);
	const uint32_t dropped = 5;
	const uint32_t total = capacity + dropped;

	for (uint32_t index = 0; index < total; index++)
	{
		feed(index);
		simulator->step();
	}

	recorder::Capture capture;
	ASSERT_TRUE(handler->download(*probe, variables, capture, error)) << error;

	ASSERT_EQ(capture.samples, capacity);
	/* The window begins where the write pointer stands, which is one sample past
	   the ones the ring has already overwritten. */
	EXPECT_EQ(capture.window.oldest, dropped * 6u);
	EXPECT_EQ(capture.series[0][0], dropped);
	EXPECT_EQ(capture.series[0][capacity - 1], dropped + capacity - 1);
}

TEST_F(RecorderRoundTrip, ArmsTheTriggerAndReadsBackFromTheTriggerPoint)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	const std::vector<recorder::Slot> variables = standardVariables();

	recorder::Config config;
	config.mode = recorder::Mode::Single;
	config.variables = variables;
	config.trigger.enabled = true;
	config.trigger.source = "counter";
	config.trigger.edge = recorder::TriggerEdge::Rising;
	config.trigger.value = 100.0;
	config.trigger.preTriggerSamples = 5;

	ASSERT_TRUE(handler->start(*probe, config, error)) << error;

	EXPECT_EQ(simulator->triggerState(), recorder::TriggerState::Ready);
	EXPECT_EQ(simulator->triggerType(), recorder::TriggerType::I32);
	EXPECT_EQ(simulator->triggerEdge(), recorder::TriggerEdge::Rising);
	EXPECT_EQ(simulator->triggerSourceAddress(), counterAddress);
	EXPECT_EQ(simulator->preTriggerSamples(), 5u);
	EXPECT_EQ(simulator->triggerValue(), 100);

	/* Values climb by ten, so the trigger is crossed at sample ten. Nothing may
	   fire before that, and the initial previous value is what decides it. */
	for (uint32_t index = 0; index < 10; index++)
	{
		feed(static_cast<uint32_t>(index) * 10);
		simulator->step();
		EXPECT_EQ(simulator->triggerState(), recorder::TriggerState::Ready) << "fired at sample " << index;
	}

	feed(100);
	simulator->step();
	EXPECT_EQ(simulator->triggerState(), recorder::TriggerState::Triggered);

	const uint32_t steps = simulator->runUntil(recorder::State::Full, 200000);
	EXPECT_GT(steps, 0u);
	EXPECT_EQ(simulator->state(), recorder::State::Full);

	recorder::Capture capture;
	ASSERT_TRUE(handler->download(*probe, variables, capture, error)) << error;

	ASSERT_EQ(capture.samples, (32768u / 6u));
	/* Five samples before the trigger, and the crossing sample at index five. */
	EXPECT_EQ(capture.ring.start, 5u * 6u);
	EXPECT_EQ(capture.window.oldest, 5u * 6u);
	EXPECT_EQ(capture.series[0][5], 100u);
	EXPECT_EQ(capture.series[0][4], 90u);
}

TEST_F(RecorderRoundTrip, LeavesTheTriggerUnarmedWhenTheSourceNameIsUnknown)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	const std::vector<recorder::Slot> variables = standardVariables();

	recorder::Config config;
	config.mode = recorder::Mode::Single;
	config.variables = variables;
	config.trigger.enabled = true;
	config.trigger.source = "not_in_this_group";
	config.trigger.value = 100.0;

	ASSERT_TRUE(handler->start(*probe, config, error)) << error;

	/* Nothing matches the name, so the effective type stays unknown and the
	   source address is cleared. The target has no case for that type, so the
	   capture behaves like a free run instead of firing on some other signal. */
	EXPECT_EQ(simulator->triggerState(), recorder::TriggerState::Ready);
	EXPECT_EQ(simulator->triggerType(), recorder::TriggerType::Unknown);
	EXPECT_EQ(simulator->triggerSourceAddress(), 0u);
}

TEST_F(RecorderRoundTrip, DownsamplesWhenAsked)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	const std::vector<recorder::Slot> variables = standardVariables();

	recorder::Config config;
	config.mode = recorder::Mode::Run;
	config.variables = variables;
	config.skippedSamples = 3;

	ASSERT_TRUE(handler->start(*probe, config, error)) << error;
	EXPECT_EQ(simulator->samplesToSkip(), 3u);

	for (uint32_t index = 0; index < 40; index++)
	{
		feed(index);
		simulator->step();
	}

	recorder::Capture capture;
	ASSERT_TRUE(handler->download(*probe, variables, capture, error)) << error;

	/* One sample kept out of every four steps of the timer interrupt. */
	ASSERT_EQ(capture.samples, 10u);
	EXPECT_NEAR(capture.samplePeriodSeconds, 4e-5, 1e-12);
}

TEST_F(RecorderRoundTrip, RefusesAGroupLargerThanTheTargetAccepts)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	recorder::Config config;
	config.mode = recorder::Mode::Run;

	for (uint32_t index = 0; index < 13; index++)
		config.variables.push_back({counterAddress + index * 4, 4, static_cast<uint8_t>(recorder::TriggerType::U32), "v"});

	EXPECT_FALSE(handler->apply(*probe, config, error));
	EXPECT_NE(error.find("12"), std::string::npos);
}

TEST_F(RecorderRoundTrip, RefusesAVariableWiderThanTheProtocolCarries)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	recorder::Config config;
	config.mode = recorder::Mode::Run;
	config.variables = {{counterAddress, 8, static_cast<uint8_t>(recorder::TriggerType::U32), "wide"}};

	EXPECT_FALSE(handler->apply(*probe, config, error));
	EXPECT_NE(error.find("wide"), std::string::npos);
}

TEST_F(RecorderRoundTrip, RefusesToCopyABufferThatBelongsToAnotherVariableSet)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	const std::vector<recorder::Slot> variables = standardVariables();

	recorder::Config config;
	config.mode = recorder::Mode::Run;
	config.variables = variables;

	ASSERT_TRUE(handler->start(*probe, config, error)) << error;

	feed(1);
	feed(2);
	feed(3);
	simulator->step();
	simulator->step();
	simulator->step();

	std::vector<recorder::Slot> narrowed = {variables[0]};

	recorder::Capture capture;
	EXPECT_FALSE(handler->download(*probe, narrowed, capture, error));
	EXPECT_NE(error.find("Start the recorder again"), std::string::npos);
}

TEST_F(RecorderRoundTrip, AnswersWithAnEmptyCaptureBeforeTheFirstSample)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	const std::vector<recorder::Slot> variables = standardVariables();

	recorder::Config config;
	config.mode = recorder::Mode::Run;
	config.variables = variables;

	ASSERT_TRUE(handler->start(*probe, config, error)) << error;

	recorder::Capture capture;
	ASSERT_TRUE(handler->download(*probe, variables, capture, error)) << error;

	EXPECT_EQ(capture.samples, 0u);
	EXPECT_TRUE(capture.series.empty() || capture.series[0].empty());
}

TEST_F(RecorderRoundTrip, RefusesToCopyBeforeTheRecorderWasEverConfigured)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	recorder::Capture capture;
	EXPECT_FALSE(handler->download(*probe, standardVariables(), capture, error));
	EXPECT_NE(error.find("Set the recorder mode"), std::string::npos);
}

TEST_F(RecorderRoundTrip, StopsTheTarget)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	recorder::Config config;
	config.mode = recorder::Mode::Run;
	config.variables = standardVariables();

	ASSERT_TRUE(handler->start(*probe, config, error)) << error;
	ASSERT_EQ(simulator->state(), recorder::State::Running);

	ASSERT_TRUE(handler->stop(*probe, error)) << error;
	EXPECT_EQ(simulator->state(), recorder::State::Init);
}

TEST_F(RecorderRoundTrip, ASerialProbeAnswersABulkReadInOneTransaction)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	recorder::Config config;
	config.mode = recorder::Mode::Run;
	config.variables = standardVariables();

	ASSERT_TRUE(handler->start(*probe, config, error)) << error;

	feed(11);
	feed(22);
	simulator->step();
	simulator->step();

	uint32_t counter = 0;
	ASSERT_TRUE(probe->readMemory(counterAddress, reinterpret_cast<uint8_t*>(&counter), 4));
	EXPECT_EQ(counter, 22u);

	uint16_t level = 0;
	ASSERT_TRUE(probe->readMemory(levelAddress, reinterpret_cast<uint8_t*>(&level), 2));
	EXPECT_EQ(level, 22u * 7u);

	/* One frame carried both, which is the whole point of reading over the line
	   rather than one value at a time. */
	const std::vector<serial::Request>& requests = target.getRequests();
	ASSERT_GE(requests.size(), 1u);

	const serial::Request& last = requests.back();
	EXPECT_EQ(last.command, serial::Command::Read);
}

TEST_F(RecorderRoundTrip, ABufferReadMovesMoreThanTheProtocolFrameCarries)
{
	ASSERT_TRUE(handler->detect(*probe, error)) << error;

	/* The length field of a frame is one byte, so a block of three hundred has
	   to go out as two frames. */
	std::vector<uint8_t> block(300, 0);

	for (uint32_t offset = 0; offset < block.size(); offset++)
		target.poke8(counterAddress + offset, static_cast<uint8_t>(offset & 0xFF));

	ASSERT_TRUE(probe->readBlock(counterAddress, block.data(), static_cast<uint32_t>(block.size())));

	EXPECT_EQ(block[0], 0u);
	EXPECT_EQ(block[255], 255u);
	EXPECT_EQ(block[256], 0u);
	EXPECT_EQ(block[299], 43u);
}
