#ifndef _RecorderLayout_HPP
#define _RecorderLayout_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/*
 * Where the fields of the target side recorder live in the target memory.
 *
 * The recorder is two globals in the firmware: a settings struct that describes
 * the build, and the recorder itself. The host never knows how the firmware was
 * compiled: it reads the settings, and the numbers in there are the same macros
 * that decide the size of the larger struct. So the offsets below can be worked
 * out at run time from two numbers.
 *
 * Layout of ____Recorder, for a target that is not a C2000:
 *
 *   0   samplesToSkip        4 bytes
 *   4   variableCount        4
 *   8   maxActualBufferSize  4
 *   12  samplesPackSize      2
 *   14  (padding)            2
 *   16  samplesSinceStart    4
 *   20  skippedSamples       4
 *   24  state                1
 *   25  (padding)            3
 *   28  sampleList[]         8 bytes per variable
 *   ..  recorderbuf.head     4
 *   ..  recorderbuf.start    4
 *   ..  recorderbuf.data[]   maxBufferSize bytes
 *   ..  trigger              48 bytes with float support, 36 without
 *
 * Only two things are not derivable and are taken as given: the element size of
 * the buffer, which is one byte except on a C2000 where it is two, and the
 * float support switch, which the settings struct does report.
 */

namespace recorder
{
	/* ____RecorderSettings, the struct the firmware publishes first so that a
	   host can find out what it is talking to. */
	struct Settings
	{
		uint32_t version = 0;
		uint32_t revision = 0;
		uint32_t timestepNs = 0;
		uint32_t maxBufferSize = 0;
		uint32_t maxVariables = 0;
		uint16_t floatSupport = 0;
	};

	/* Five 32 bit fields and one 16 bit field, rounded up to the alignment of
	   the struct's widest member. */
	constexpr size_t settingsSize = 24;

	/* The versions this host knows how to talk to. A newer major version may
	   have moved a field, and reading a struct at the wrong offsets produces
	   numbers rather than an error. */
	constexpr uint32_t supportedVersion = 8;
	constexpr uint32_t supportedRevision = 3;

	Settings parseSettings(const uint8_t* data, size_t length);

	std::string describe(const Settings& settings);

	/* Runtime offsets of the recorder fields, in target memory. */
	struct Layout
	{
		uint32_t maxVariables = 0;
		uint32_t maxBufferSize = 0;
		bool floatSupport = true;
		/* One byte per buffer element, except on a C2000 where the buffer is an
		   array of 16 bit words. */
		uint32_t elementSize = 1;

		uint32_t samplesToSkipOffset = 0;
		uint32_t variableCountOffset = 4;
		uint32_t maxActualBufferSizeOffset = 8;
		uint32_t samplesPackSizeOffset = 12;
		uint32_t samplesSinceStartOffset = 16;
		uint32_t skippedSamplesOffset = 20;
		uint32_t stateOffset = 24;
		uint32_t sampleListOffset = 28;
		uint32_t bufferHeadOffset = 0;
		uint32_t bufferStartOffset = 0;
		uint32_t bufferDataOffset = 0;
		uint32_t triggerOffset = 0;
		uint32_t triggerStateOffset = 0;
		uint32_t triggerEdgeOffset = 0;
		uint32_t triggerTypeOffset = 0;
		uint32_t triggerSourceAddressOffset = 0;
		uint32_t triggerPreTriggerSamplesOffset = 0;
		/* The trigger keeps three separate views of the same threshold, one per
		   comparison the target can do, and each view has its own current and
		   previous value next to it. */
		uint32_t triggerValueOffset = 0;
		uint32_t triggerActValueOffset = 0;
		uint32_t triggerPrevValueOffset = 0;
		uint32_t triggerValueU32Offset = 0;
		uint32_t triggerActValueU32Offset = 0;
		uint32_t triggerPrevValueU32Offset = 0;
		uint32_t triggerValueFloatOffset = 0;
		uint32_t triggerActValueFloatOffset = 0;
		uint32_t triggerPrevValueFloatOffset = 0;

		/* Bytes of target memory the recorder occupies. */
		uint32_t totalSize = 0;
		/* Bytes the buffer occupies, which is maxBufferSize elements. */
		uint32_t bufferByteSize = 0;
	};

	Layout makeLayout(const Settings& settings, uint32_t elementSize = 1);

	/* Number of bytes one sample of the given variables occupies. */
	uint32_t packSizeOf(const std::vector<uint32_t>& variableSizes);
}  // namespace recorder

#endif
