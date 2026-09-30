#ifndef _SerialTargetSimulator_HPP
#define _SerialTargetSimulator_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "RecorderLayout.hpp"
#include "SerialProtocol.hpp"
#include "SerialTransport.hpp"

/*
 * A stand in for the firmware side, so that the host side of the serial channel
 * and of the recorder can be exercised without a board on the desk.
 *
 * Two pieces, both ports of the target sources rather than descriptions of them:
 *
 *   SerialTargetSimulator holds memory and answers frames the way serialDriver.h
 *   does, including the parts that are easy to get wrong: a reply has no start
 *   marker, a read reply ends with the checksum of the values, a write reply is
 *   one status byte, a buffer read reply is raw bytes with nothing appended.
 *
 *   RecorderSimulator holds a ____Recorder image laid out by the same code the
 *   host uses, and advances it the way recorder.h does, so a test can start a
 *   capture, feed samples and then check what the host reconstructs from it.
 *
 * Where the two disagree with the reference, the reference wins; the ports below
 * follow the switch statements line by line, including the order in which the
 * sample is written before the ring state is updated.
 */

class SerialTargetSimulator : public serial::ITransport
{
   public:
	static constexpr uint32_t memoryBase = 0x20000000;
	static constexpr uint32_t memorySize = 0x00040000;

	/* ---- memory ---- */

	uint8_t* at(uint32_t address)
	{
		if (address < memoryBase || address - memoryBase >= memorySize)
			return nullptr;

		return memory.data() + (address - memoryBase);
	}

	void poke8(uint32_t address, uint8_t value)
	{
		if (uint8_t* cell = at(address))
			*cell = value;
	}

	void poke16(uint32_t address, uint16_t value)
	{
		for (size_t byte = 0; byte < 2; byte++)
			poke8(address + static_cast<uint32_t>(byte), static_cast<uint8_t>((value >> (8 * byte)) & 0xFF));
	}

	void poke32(uint32_t address, uint32_t value)
	{
		for (size_t byte = 0; byte < 4; byte++)
			poke8(address + static_cast<uint32_t>(byte), static_cast<uint8_t>((value >> (8 * byte)) & 0xFF));
	}

	uint32_t peek32(uint32_t address) const
	{
		uint32_t value = 0;

		for (size_t byte = 0; byte < 4; byte++)
		{
			const uint32_t offset = address - memoryBase + static_cast<uint32_t>(byte);

			if (offset < memorySize)
				value |= static_cast<uint32_t>(memory[offset]) << (8 * byte);
		}

		return value;
	}

	uint16_t peek16(uint32_t address) const
	{
		uint16_t value = 0;

		for (size_t byte = 0; byte < 2; byte++)
		{
			const uint32_t offset = address - memoryBase + static_cast<uint32_t>(byte);

			if (offset < memorySize)
				value |= static_cast<uint16_t>(static_cast<uint16_t>(memory[offset]) << (8 * byte));
		}

		return value;
	}

	uint8_t peek8(uint32_t address) const
	{
		const uint32_t offset = address - memoryBase;
		return offset < memorySize ? memory[offset] : 0;
	}

	/* ---- the line ---- */

	bool open(const std::string&, uint32_t) override
	{
		openFlag = true;
		transmit.clear();
		return true;
	}

	void close() override { openFlag = false; }

	bool isOpen() const override { return openFlag; }

	bool write(const uint8_t* data, size_t size) override
	{
		for (size_t index = 0; index < size; index++)
		{
			/* A byte the driver cannot make sense of leaves it waiting for a new
			   start of frame, exactly as on the target. */
			if (std::optional<serial::Request> request = parser.push(data[index]))
				serve(*request);
		}

		return true;
	}

	size_t read(uint8_t* data, size_t size, uint32_t) override
	{
		const size_t available = std::min(size, transmit.size());

		for (size_t index = 0; index < available; index++)
		{
			data[index] = transmit.front();
			transmit.erase(transmit.begin());
		}

		return available;
	}

	/* The host reads exactly the number of bytes a reply carries, so there is
	   never a leftover to throw away. */
	void discardInput() override {}

	std::string getLastError() const override { return {}; }

	/* ---- what came in ---- */

	const std::vector<serial::Request>& getRequests() const { return requests; }
	uint32_t getCrcErrorCount() const { return parser.getCrcErrorCount(); }
	uint32_t getDroppedFrameCount() const { return parser.getDroppedFrameCount(); }

   private:
	void serve(const serial::Request& request)
	{
		requests.push_back(request);

		switch (request.command)
		{
			case serial::Command::Write:
			{
				/* The driver copies the value bytes onto the address and answers
				   with a single status byte, without a checksum. */
				for (size_t index = 0; index < request.payload.size(); index++)
					poke8(request.address + static_cast<uint32_t>(index), request.payload[index]);

				transmit.push_back(serial::responseOk);
				break;
			}

			case serial::Command::BufferRead:
			{
				for (uint32_t offset = 0; offset < request.bufferReadSize; offset++)
				{
					const uint8_t* source = at(request.address + offset);
					transmit.push_back(source != nullptr ? *source : 0);
				}

				break;
			}

			case serial::Command::Read:
			{
				std::vector<uint8_t> values;

				for (const serial::ReadEntry& entry : request.entries)
				{
					for (uint32_t offset = 0; offset < entry.size; offset++)
					{
						const uint8_t* source = at(entry.address + offset);
						values.push_back(source != nullptr ? *source : 0);
					}
				}

				const uint16_t checksum = serial::crc16(values);

				transmit.insert(transmit.end(), values.begin(), values.end());
				transmit.push_back(static_cast<uint8_t>(checksum & 0xFF));
				transmit.push_back(static_cast<uint8_t>((checksum >> 8) & 0xFF));
				break;
			}
		}
	}

	bool openFlag = false;
	std::vector<uint8_t> memory = std::vector<uint8_t>(memorySize, 0);
	std::vector<uint8_t> transmit;
	std::vector<serial::Request> requests;
	serial::RequestParser parser;
};

/*
 * The recorder as the firmware runs it. The image is byte for byte what the host
 * addresses, so a test proves something about the pair and not just about one
 * side of it.
 */
class RecorderSimulator
{
   public:
	RecorderSimulator(SerialTargetSimulator& target, uint32_t settingsAddress, uint32_t recorderAddress, const recorder::Settings& settings, uint32_t elementSize = 1)
		: target(target), settingsAddress(settingsAddress), recorderAddress(recorderAddress), settings(settings), layout(recorder::makeLayout(settings, elementSize))
	{
		install();
	}

	/* Writes the settings struct and an empty recorder into target memory. */
	void install()
	{
		for (size_t byte = 0; byte < 4; byte++)
		{
			target.poke8(settingsAddress + static_cast<uint32_t>(byte), static_cast<uint8_t>((settings.version >> (8 * byte)) & 0xFF));
			target.poke8(settingsAddress + 4 + static_cast<uint32_t>(byte), static_cast<uint8_t>((settings.revision >> (8 * byte)) & 0xFF));
			target.poke8(settingsAddress + 8 + static_cast<uint32_t>(byte), static_cast<uint8_t>((settings.timestepNs >> (8 * byte)) & 0xFF));
			target.poke8(settingsAddress + 12 + static_cast<uint32_t>(byte), static_cast<uint8_t>((settings.maxBufferSize >> (8 * byte)) & 0xFF));
			target.poke8(settingsAddress + 16 + static_cast<uint32_t>(byte), static_cast<uint8_t>((settings.maxVariables >> (8 * byte)) & 0xFF));
		}

		target.poke16(settingsAddress + 20, settings.floatSupport);

		for (uint32_t offset = 0; offset < layout.totalSize; offset++)
			target.poke8(recorderAddress + offset, 0);
	}

	/* The fields the host wrote, read back the way the target would see them. */
	recorder::State state() const { return static_cast<recorder::State>(target.peek8(recorderAddress + layout.stateOffset)); }
	recorder::TriggerState triggerState() const { return static_cast<recorder::TriggerState>(target.peek8(recorderAddress + layout.triggerStateOffset)); }
	recorder::TriggerType triggerType() const { return static_cast<recorder::TriggerType>(target.peek8(recorderAddress + layout.triggerTypeOffset)); }
	recorder::TriggerEdge triggerEdge() const { return static_cast<recorder::TriggerEdge>(target.peek8(recorderAddress + layout.triggerEdgeOffset)); }
	uint32_t samplesToSkip() const { return target.peek32(recorderAddress + layout.samplesToSkipOffset); }
	uint32_t variableCount() const { return target.peek32(recorderAddress + layout.variableCountOffset); }
	uint32_t maxActualBufferSize() const { return target.peek32(recorderAddress + layout.maxActualBufferSizeOffset); }
	uint16_t packSize() const { return target.peek16(recorderAddress + layout.samplesPackSizeOffset); }
	uint32_t head() const { return target.peek32(recorderAddress + layout.bufferHeadOffset); }
	uint32_t start() const { return target.peek32(recorderAddress + layout.bufferStartOffset); }
	uint32_t samplesSinceStart() const { return target.peek32(recorderAddress + layout.samplesSinceStartOffset); }
	uint32_t triggerSourceAddress() const { return target.peek32(recorderAddress + layout.triggerSourceAddressOffset); }
	uint32_t preTriggerSamples() const { return target.peek32(recorderAddress + layout.triggerPreTriggerSamplesOffset); }
	int32_t triggerValue() const { return static_cast<int32_t>(target.peek32(recorderAddress + layout.triggerValueOffset)); }
	uint32_t triggerValueU32() const { return target.peek32(recorderAddress + layout.triggerValueU32Offset); }
	int32_t previousValue() const { return static_cast<int32_t>(target.peek32(recorderAddress + layout.triggerPrevValueOffset)); }
	uint32_t variableAddress(size_t index) const { return target.peek32(recorderAddress + layout.sampleListOffset + static_cast<uint32_t>(index) * 8); }
	uint32_t variableSize(size_t index) const { return target.peek32(recorderAddress + layout.sampleListOffset + static_cast<uint32_t>(index) * 8 + 4); }

	const recorder::Layout& getLayout() const { return layout; }

	/* One call of the timer interrupt, ported from ____sampleVariables() and
	   recorderStep(). Skipping is handled the way recorderStep does, so a
	   downsampled run advances the counter without writing anything. */
	void step()
	{
		if (state() != recorder::State::Running)
		{
			target.poke32(recorderAddress + layout.samplesSinceStartOffset, 0);
			return;
		}

		const uint32_t skipped = target.peek32(recorderAddress + layout.skippedSamplesOffset);

		if (skipped < samplesToSkip())
		{
			target.poke32(recorderAddress + layout.skippedSamplesOffset, skipped + 1);
			return;
		}

		sampleVariables();

		target.poke32(recorderAddress + layout.samplesSinceStartOffset, samplesSinceStart() + 1);
		target.poke32(recorderAddress + layout.skippedSamplesOffset, 0);
	}

	/* Runs the target until it reports the given state, so a test does not have
	   to know how many samples a buffer holds. */
	uint32_t runUntil(recorder::State wanted, uint32_t limit)
	{
		uint32_t steps = 0;

		while (state() != wanted && steps < limit)
		{
			step();
			steps++;
		}

		return steps;
	}

	uint32_t runFor(uint32_t steps)
	{
		for (uint32_t index = 0; index < steps; index++)
			step();

		return steps;
	}

   private:
	uint32_t readField32(uint32_t offset) const { return target.peek32(recorderAddress + offset); }

	void writeField32(uint32_t offset, uint32_t value) { target.poke32(recorderAddress + offset, value); }

	void sampleVariables()
	{
		const uint32_t count = variableCount();
		const uint16_t pack = packSize();
		const uint32_t maxActual = maxActualBufferSize();

		uint32_t writeHead = head();

		for (uint32_t index = 0; index < count; index++)
		{
			const uint32_t address = variableAddress(index);
			const uint32_t size = variableSize(index);

			for (uint32_t byte = 0; byte < size; byte++)
			{
				const uint8_t* source = target.at(address + byte);
				target.poke8(recorderAddress + layout.bufferDataOffset + writeHead + byte, source != nullptr ? *source : 0);
			}

			writeHead += size;
		}

		writeField32(layout.bufferHeadOffset, writeHead);

		switch (triggerState())
		{
			case recorder::TriggerState::Setup:
			{
				writeField32(layout.bufferStartOffset, 0);

				if (writeHead + pack > maxActual)
				{
					writeField32(layout.bufferHeadOffset, 0);
					writeField32(layout.stateOffset, static_cast<uint32_t>(recorder::State::Full));
				}

				break;
			}

			case recorder::TriggerState::Ready:
			{
				writeField32(layout.bufferStartOffset, 0);

				if (writeHead + pack > maxActual)
				{
					writeField32(layout.bufferHeadOffset, 0);
					writeField32(layout.bufferStartOffset, 0);
				}

				writeField32(layout.triggerPrevValueOffset, readField32(layout.triggerActValueOffset));

				switch (triggerType())
				{
					case recorder::TriggerType::U8:
						writeField32(layout.triggerActValueOffset, target.peek8(triggerSourceAddress()));
						compareSigned();
						break;
					case recorder::TriggerType::I8:
						writeField32(layout.triggerActValueOffset, static_cast<int32_t>(static_cast<int8_t>(target.peek8(triggerSourceAddress()))));
						compareSigned();
						break;
					case recorder::TriggerType::U16:
						writeField32(layout.triggerActValueOffset, target.peek16(triggerSourceAddress()));
						compareSigned();
						break;
					case recorder::TriggerType::I16:
						writeField32(layout.triggerActValueOffset, static_cast<int32_t>(static_cast<int16_t>(target.peek16(triggerSourceAddress()))));
						compareSigned();
						break;
					case recorder::TriggerType::U32:
						writeField32(layout.triggerPrevValueU32Offset, readField32(layout.triggerActValueU32Offset));
						writeField32(layout.triggerActValueU32Offset, target.peek32(triggerSourceAddress()));
						compareUnsigned();
						break;
					case recorder::TriggerType::I32:
						writeField32(layout.triggerActValueOffset, static_cast<int32_t>(target.peek32(triggerSourceAddress())));
						compareSigned();
						break;
					case recorder::TriggerType::F32:
					{
						float value = 0.0f;
						uint32_t bits = target.peek32(triggerSourceAddress());
						std::memcpy(&value, &bits, sizeof(value));
						writeField32(layout.triggerPrevValueFloatOffset, readField32(layout.triggerActValueFloatOffset));
						writeField32(layout.triggerActValueFloatOffset, bits);
						compareFloat(value);
						break;
					}
					default:
						break;
				}

				break;
			}

			case recorder::TriggerState::Triggered:
			{
				const uint32_t currentStart = readField32(layout.bufferStartOffset);

				if (writeHead + pack > maxActual)
				{
					writeField32(layout.triggerStateOffset, static_cast<uint32_t>(recorder::TriggerState::Posttrigger));
					writeField32(layout.bufferHeadOffset, 0);
				}
				else if (writeHead + pack > currentStart && writeHead <= currentStart)
				{
					writeField32(layout.triggerStateOffset, static_cast<uint32_t>(recorder::TriggerState::Posttrigger));
					writeField32(layout.stateOffset, static_cast<uint32_t>(recorder::State::Full));
					writeField32(layout.bufferHeadOffset, 0);
				}

				break;
			}

			case recorder::TriggerState::Posttrigger:
			{
				if (writeHead + pack > readField32(layout.bufferStartOffset))
				{
					writeField32(layout.stateOffset, static_cast<uint32_t>(recorder::State::Full));
					writeField32(layout.bufferHeadOffset, 0);
				}

				break;
			}
		}
	}

	/* The three comparisons of the reference. The trigger value lives at a fixed
	   offset for the signed view and the unsigned view is the pair that follows
	   it, which is why the U32 case above copies between them. */
	void compareSigned()
	{
		const int32_t actual = static_cast<int32_t>(readField32(layout.triggerActValueOffset));
		const int32_t previous = static_cast<int32_t>(readField32(layout.triggerPrevValueOffset));
		const int32_t threshold = static_cast<int32_t>(readField32(layout.triggerValueOffset));

		if (triggerEdge() == recorder::TriggerEdge::Rising)
		{
			if (actual >= threshold && previous < threshold)
				fire();
		}
		else if (actual <= threshold && previous > threshold)
			fire();
	}

	void compareUnsigned()
	{
		const uint32_t actual = readField32(layout.triggerActValueU32Offset);
		const uint32_t previous = readField32(layout.triggerPrevValueU32Offset);
		const uint32_t threshold = readField32(layout.triggerValueU32Offset);

		if (triggerEdge() == recorder::TriggerEdge::Rising)
		{
			if (actual >= threshold && previous < threshold)
				fire();
		}
		else if (actual <= threshold && previous > threshold)
			fire();
	}

	void compareFloat(float actual)
	{
		float previous = 0.0f;
		float threshold = 0.0f;
		uint32_t bits = readField32(layout.triggerPrevValueFloatOffset);
		std::memcpy(&previous, &bits, sizeof(previous));
		bits = readField32(layout.triggerValueFloatOffset);
		std::memcpy(&threshold, &bits, sizeof(threshold));

		if (triggerEdge() == recorder::TriggerEdge::Rising)
		{
			if (actual >= threshold && previous < threshold)
				fire();
		}
		else if (actual <= threshold && previous > threshold)
			fire();
	}

	void fire()
	{
		if (samplesSinceStart() < preTriggerSamples())
			return;

		writeField32(layout.triggerStateOffset, static_cast<uint32_t>(recorder::TriggerState::Triggered));

		const int64_t index = static_cast<int64_t>(head()) - (static_cast<int64_t>(preTriggerSamples()) + 1) * packSize();
		const int64_t modulo = maxActualBufferSize();

		writeField32(layout.bufferStartOffset, static_cast<uint32_t>(index < 0 ? modulo + index : index));
	}

	SerialTargetSimulator& target;
	uint32_t settingsAddress;
	uint32_t recorderAddress;
	recorder::Settings settings;
	recorder::Layout layout;
};

#endif
