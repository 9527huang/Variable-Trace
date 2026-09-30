#include "RecorderLayout.hpp"

#include <sstream>

namespace
{
	uint32_t readU32(const uint8_t* data)
	{
		return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
			   (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
	}

	uint16_t readU16(const uint8_t* data)
	{
		return static_cast<uint16_t>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
	}
}  // namespace

namespace recorder
{
	Settings parseSettings(const uint8_t* data, size_t length)
	{
		Settings settings;

		if (data == nullptr || length < settingsSize)
			return settings;

		settings.version = readU32(data + 0);
		settings.revision = readU32(data + 4);
		settings.timestepNs = readU32(data + 8);
		settings.maxBufferSize = readU32(data + 12);
		settings.maxVariables = readU32(data + 16);
		settings.floatSupport = readU16(data + 20);

		return settings;
	}

	std::string describe(const Settings& settings)
	{
		std::ostringstream text;
		text << "recorder " << settings.version << "." << settings.revision
			 << ", " << settings.maxVariables << " variables, "
			 << settings.maxBufferSize << " samples, timestep " << settings.timestepNs << " ns"
			 << (settings.floatSupport != 0 ? ", float triggers" : ", integer triggers only");
		return text.str();
	}

	Layout makeLayout(const Settings& settings, uint32_t elementSize)
	{
		Layout layout;

		layout.maxVariables = settings.maxVariables;
		layout.maxBufferSize = settings.maxBufferSize;
		layout.floatSupport = settings.floatSupport != 0;
		layout.elementSize = elementSize == 0 ? 1 : elementSize;

		/* ____Variable is two 32 bit fields. */
		const uint32_t sampleListSize = layout.maxVariables * 8;

		layout.bufferHeadOffset = layout.sampleListOffset + sampleListSize;
		layout.bufferStartOffset = layout.bufferHeadOffset + 4;
		layout.bufferDataOffset = layout.bufferHeadOffset + 8;
		layout.bufferByteSize = layout.maxBufferSize * layout.elementSize;
		layout.triggerOffset = layout.bufferDataOffset + layout.bufferByteSize;

		layout.triggerStateOffset = layout.triggerOffset + 0;
		layout.triggerEdgeOffset = layout.triggerOffset + 1;
		layout.triggerTypeOffset = layout.triggerOffset + 2;
		layout.triggerSourceAddressOffset = layout.triggerOffset + 4;
		layout.triggerPreTriggerSamplesOffset = layout.triggerOffset + 8;
		layout.triggerValueOffset = layout.triggerOffset + 12;
		layout.triggerActValueOffset = layout.triggerOffset + 16;
		layout.triggerPrevValueOffset = layout.triggerOffset + 20;
		layout.triggerValueU32Offset = layout.triggerOffset + 24;
		layout.triggerActValueU32Offset = layout.triggerOffset + 28;
		layout.triggerPrevValueU32Offset = layout.triggerOffset + 32;
		layout.triggerValueFloatOffset = layout.triggerOffset + 36;
		layout.triggerActValueFloatOffset = layout.triggerOffset + 40;
		layout.triggerPrevValueFloatOffset = layout.triggerOffset + 44;

		/* Three enum members then a pad, then eight 32 bit values, then three
		   floats that are only there when float support is on. */
		const uint32_t triggerSize = layout.floatSupport ? 48 : 36;
		layout.totalSize = layout.triggerOffset + triggerSize;

		return layout;
	}

	uint32_t packSizeOf(const std::vector<uint32_t>& variableSizes)
	{
		uint32_t total = 0;

		for (uint32_t size : variableSizes)
			total += size;

		return total;
	}
}  // namespace recorder
