#include "RecorderHandler.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>

#include "PlotGroupHandler.hpp"

namespace
{
	std::string hexAddress(uint32_t address)
	{
		std::ostringstream text;
		text << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << address;
		return text.str();
	}

	uint16_t readU16(const uint8_t* data)
	{
		return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
	}

	uint32_t readU32(const uint8_t* data)
	{
		return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
			   (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
	}

	void appendU32(std::vector<uint8_t>& out, uint32_t value)
	{
		out.push_back(static_cast<uint8_t>(value & 0xFF));
		out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
		out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
		out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
	}
}  // namespace

namespace recorder
{
	std::string modeToString(Mode mode)
	{
		switch (mode)
		{
			case Mode::Run:
				return "run";
			case Mode::Normal:
				return "normal";
			case Mode::Single:
				return "single";
		}

		return "run";
	}

	bool modeFromString(const std::string& name, Mode& mode)
	{
		if (name == "run")
		{
			mode = Mode::Run;
			return true;
		}

		if (name == "normal")
		{
			mode = Mode::Normal;
			return true;
		}

		if (name == "single")
		{
			mode = Mode::Single;
			return true;
		}

		return false;
	}

	std::string triggerTypeToString(TriggerType type)
	{
		switch (type)
		{
			case TriggerType::Unknown:
				return "unknown";
			case TriggerType::U8:
				return "u8";
			case TriggerType::I8:
				return "i8";
			case TriggerType::U16:
				return "u16";
			case TriggerType::I16:
				return "i16";
			case TriggerType::U32:
				return "u32";
			case TriggerType::I32:
				return "i32";
			case TriggerType::F32:
				return "f32";
		}

		return "unknown";
	}

	std::string triggerEdgeToString(TriggerEdge edge)
	{
		return edge == TriggerEdge::Falling ? "falling" : "rising";
	}

	uint32_t packSizeOf(const std::vector<Slot>& variables)
	{
		uint32_t total = 0;

		for (const Slot& slot : variables)
			total += slot.size;

		return total;
	}

	bool configFromGroup(const PlotGroup& group, Config& out, std::string& error)
	{
		out.variables.clear();

		/* A set keyed on the name keeps the first occurrence of a variable that
		   two plots of the group share. The flag is also read from here, so the
		   value the trigger compares has a type even when its plot is hidden. */
		std::set<std::string> seen;

		for (auto iterator = group.begin(); iterator != group.end(); ++iterator)
		{
			Plot* plot = iterator->second.plot.get();

			if (plot == nullptr)
				continue;

			for (const auto& entry : plot->getSeriesMap())
			{
				Variable* variable = entry.second ? entry.second->var : nullptr;

				if (variable == nullptr)
					continue;

				const std::string name = variable->getName();

				if (!seen.insert(name).second)
					continue;

				Slot slot;
				slot.address = variable->getAddress();
				slot.size = variable->getSize();
				slot.type = static_cast<uint8_t>(variable->getType());
				slot.name = name;
				out.variables.push_back(std::move(slot));
			}
		}

		if (out.variables.empty())
		{
			error = "The recorder group '" + group.getName() + "' holds no variables. Add a plot with a variable to it first.";
			return false;
		}

		for (const Slot& slot : out.variables)
		{
			if (slot.size == 0 || slot.size > 4)
			{
				error = "The variable '" + slot.name + "' is " + std::to_string(slot.size) + " bytes wide, and a sample carries one to four bytes per variable.";
				return false;
			}
		}

		error.clear();
		return true;
	}

	std::vector<std::pair<uint32_t, uint32_t>> validRanges(const RingWindow& window, uint32_t modulo)
	{
		std::vector<std::pair<uint32_t, uint32_t>> ranges;

		if (window.bytes == 0 || modulo == 0)
			return ranges;

		const uint32_t oldest = window.oldest % modulo;
		const uint32_t beforeTheWrap = std::min(window.bytes, modulo - oldest);
		ranges.emplace_back(oldest, beforeTheWrap);

		if (beforeTheWrap < window.bytes)
			ranges.emplace_back(0, window.bytes - beforeTheWrap);

		return ranges;
	}

	RingWindow ringWindowOf(const RingState& ring)
	{
		RingWindow window;

		const uint32_t modulo = ring.maxActualBufferSize;

		if (modulo == 0 || ring.packSize == 0)
			return window;

		if (ring.state == State::Full)
		{
			/* The sweep ended exactly where it started, so every byte of the
			   buffer holds samples. The oldest one is where the trigger recorded
			   it, or the beginning of the buffer when there was no trigger. */
			window.oldest = ring.start % modulo;
			window.bytes = modulo - (modulo % ring.packSize);
			return window;
		}

		if (ring.start != 0)
		{
			/* A trigger pinned the oldest sample and the write pointer runs after
			   it, so the distance between the two is what holds samples. */
			window.oldest = ring.start % modulo;
			window.bytes = (ring.head + modulo - window.oldest) % modulo;
			window.bytes -= window.bytes % ring.packSize;
			return window;
		}

		/* Without a trigger the buffer fills from the beginning and then wraps.
		   The write pointer alone cannot separate "the buffer is full" from "the
		   sampling has not started", because both leave it at the same value.
		   The sample counter is what tells them apart. */
		const uint64_t written = static_cast<uint64_t>(ring.samplesSinceStart) * ring.packSize;
		window.bytes = static_cast<uint32_t>(std::min<uint64_t>(written, modulo));
		window.bytes -= window.bytes % ring.packSize;
		window.oldest = (ring.head + modulo - window.bytes) % modulo;

		return window;
	}

	bool decodeRing(const RingState& ring, const std::vector<Slot>& variables, const std::vector<uint8_t>& buffer,
					uint32_t timestepNs, uint32_t samplesToSkip, Capture& out, std::string& error)
	{
		if (ring.packSize == 0)
		{
			error = "The target reports a sample of zero bytes, so the recorder is not configured.";
			return false;
		}

		uint32_t expectedPack = 0;

		for (const Slot& slot : variables)
			expectedPack += slot.size;

		if (expectedPack != ring.packSize)
		{
			error = "The buffer holds samples of " + std::to_string(ring.packSize) + " bytes and the group adds up to " + std::to_string(expectedPack) + ", so the bytes cannot be split into variables.";
			return false;
		}

		const RingWindow window = ringWindowOf(ring);

		if (buffer.size() < window.bytes)
		{
			error = "The buffer holds " + std::to_string(window.bytes) + " bytes of samples but only " + std::to_string(buffer.size()) + " were passed in.";
			return false;
		}

		const uint32_t samples = window.bytes / ring.packSize;

		out.ring = ring;
		out.window = window;
		out.samples = samples;
		out.samplePeriodSeconds = (static_cast<double>(timestepNs) * (static_cast<double>(samplesToSkip) + 1.0)) / 1e9;
		out.timestamps.assign(samples, 0.0);
		out.series.assign(variables.size(), std::vector<uint32_t>(samples, 0));

		for (uint32_t sample = 0; sample < samples; sample++)
		{
			out.timestamps[sample] = static_cast<double>(sample) * out.samplePeriodSeconds;

			uint32_t offset = sample * ring.packSize;

			for (size_t index = 0; index < variables.size(); index++)
			{
				uint32_t value = 0;

				for (uint32_t byte = 0; byte < variables[index].size; byte++)
					value |= static_cast<uint32_t>(buffer[offset + byte]) << (8 * byte);

				out.series[index][sample] = value;
				offset += variables[index].size;
			}
		}

		return true;
	}

	RecorderHandler::RecorderHandler(spdlog::logger* logger) : logger(logger)
	{
	}

	void RecorderHandler::setSymbols(uint32_t settingsAddress, uint32_t recorderAddress)
	{
		/* A layout belongs to one target, so what was detected about the previous
		   one is dropped rather than carried over. */
		if (settingsAddress != this->settingsAddress || recorderAddress != this->recorderAddress)
			forgetDetection();

		this->settingsAddress = settingsAddress;
		this->recorderAddress = recorderAddress;
	}

	void RecorderHandler::clearSymbols()
	{
		setSymbols(0, 0);
	}

	void RecorderHandler::forgetDetection()
	{
		detected = false;
		detectedSettings = Settings{};
		layout = Layout{};

		/* The addresses point at a target this object no longer knows anything
		   about, so a run that was in progress cannot be continued either. */
		running = false;
	}

	void RecorderHandler::setDriverSymbol(uint32_t settingsAddress)
	{
		/* Like the recorder, what was read belongs to the target it was read
		   from, so a different address means the previous answer is void. */
		if (settingsAddress != driverSettingsAddress)
			forgetDriverDetection();

		driverSettingsAddress = settingsAddress;
	}

	void RecorderHandler::forgetDriverDetection()
	{
		driverDetected = false;
		driverSettings = serial::DriverSettings{};
	}

	bool RecorderHandler::detectDriver(IDebugProbe& probe, std::string& error)
	{
		if (driverSettingsAddress == 0)
		{
			error = "____serialDriverSettings was not found in the symbol file, so the target does not carry the serial driver. This is expected when the link is a hardware debug probe.";
			return false;
		}

		uint8_t raw[serial::driverSettingsSize] = {};

		if (!readAt(probe, driverSettingsAddress, raw, static_cast<uint32_t>(sizeof(raw)), error))
			return false;

		const serial::DriverSettings settings = serial::parseDriverSettings(raw, sizeof(raw));

		if (settings.version == 0)
		{
			error = "The target at " + hexAddress(driverSettingsAddress) + " reports serial driver version 0, which no driver writes. Check that the symbol really is ____serialDriverSettings.";
			return false;
		}

		driverSettings = settings;
		driverDetected = true;
		error.clear();

		return true;
	}

	bool RecorderHandler::readAt(IDebugProbe& probe, uint32_t address, uint8_t* data, uint32_t size, std::string& error)
	{
		if (size == 0)
			return true;

		if (!probe.readBlock(address, data, size))
		{
			error = "Reading " + std::to_string(size) + " bytes at " + hexAddress(address) + " failed: " + probe.getLastErrorMsg();
			return false;
		}

		return true;
	}

	bool RecorderHandler::writeAt(IDebugProbe& probe, uint32_t address, const uint8_t* data, uint32_t size, std::string& error)
	{
		/* A write carries four bytes at most, and a trigger threshold or a sample
		   list entry may be that long on its own, so longer values are split. */
		uint32_t written = 0;

		while (written < size)
		{
			const uint32_t chunk = std::min<uint32_t>(4, size - written);

			if (!probe.writeMemory(address + written, const_cast<uint8_t*>(data + written), chunk))
			{
				error = "Writing " + std::to_string(size) + " bytes at " + hexAddress(address + written) + " failed: " + probe.getLastErrorMsg();
				return false;
			}

			written += chunk;
		}

		return true;
	}

	bool RecorderHandler::writeU32(IDebugProbe& probe, uint32_t address, uint32_t value, std::string& error)
	{
		std::vector<uint8_t> bytes;
		appendU32(bytes, value);
		return writeAt(probe, address, bytes.data(), static_cast<uint32_t>(bytes.size()), error);
	}

	bool RecorderHandler::writeU16(IDebugProbe& probe, uint32_t address, uint16_t value, std::string& error)
	{
		const uint8_t bytes[2] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF)};
		return writeAt(probe, address, bytes, 2, error);
	}

	bool RecorderHandler::detect(IDebugProbe& probe, std::string& error)
	{
		if (!hasSymbols())
		{
			error = "Neither ____recorder nor ____recorderSettings was found in the symbol file, so there is nothing to detect. Import the variables from an *.elf/*.axf built with the recorder sources.";
			return false;
		}

		uint8_t raw[settingsSize] = {};

		if (!readAt(probe, settingsAddress, raw, static_cast<uint32_t>(settingsSize), error))
			return false;

		const Settings settings = parseSettings(raw, sizeof(raw));

		if (settings.maxVariables == 0 || settings.maxBufferSize == 0)
		{
			error = "The target at " + hexAddress(settingsAddress) + " reports " + std::to_string(settings.maxVariables) + " variables and a buffer of " + std::to_string(settings.maxBufferSize) + " elements, which cannot be right. Check that the symbol really is ____recorderSettings.";
			return false;
		}

		if (settings.version != supportedVersion || settings.revision > supportedRevision)
		{
			error = "The target runs recorder " + std::to_string(settings.version) + "." + std::to_string(settings.revision) + ", this build speaks " + std::to_string(supportedVersion) + "." + std::to_string(supportedRevision) + " and older. Update the recorder sources on the target.";
			return false;
		}

		detectedSettings = settings;
		layout = makeLayout(settings);
		detected = true;
		error.clear();

		logger->info("Recorder detected at {}: {}", hexAddress(recorderAddress), describe(settings));

		return true;
	}

	bool RecorderHandler::writeRingReset(IDebugProbe& probe, std::string& error)
	{
		if (!writeU32(probe, recorderAddress + layout.bufferHeadOffset, 0, error))
			return false;

		if (!writeU32(probe, recorderAddress + layout.bufferStartOffset, 0, error))
			return false;

		if (!writeU32(probe, recorderAddress + layout.samplesSinceStartOffset, 0, error))
			return false;

		if (!writeU32(probe, recorderAddress + layout.skippedSamplesOffset, 0, error))
			return false;

		return true;
	}

	bool RecorderHandler::writeTrigger(IDebugProbe& probe, const Config& config, TriggerState triggerState, std::string& error)
	{
		/* The source is named, not numbered, so that reordering the variable
		   list does not silently move the trigger onto a different signal. Look
		   the name up and arm only when it matches something in the list. */
		const Slot* sourcePtr = nullptr;

		if (!config.trigger.source.empty())
		{
			for (const Slot& slot : config.variables)
			{
				if (slot.name == config.trigger.source)
				{
					sourcePtr = &slot;
					break;
				}
			}
		}

		/* The trigger is only armed in the two triggered modes. In run mode the
		   effective type is left unknown, which is the value the target's own
		   switch has no case for, so no comparison ever fires and the buffer just
		   keeps the newest samples. */
		const bool armed = config.mode != Mode::Run && config.trigger.enabled && sourcePtr != nullptr;

		const Slot& source = armed ? *sourcePtr : Slot{};
		const TriggerType type = armed ? static_cast<TriggerType>(source.type) : TriggerType::Unknown;
		const TriggerEdge edge = armed ? config.trigger.edge : TriggerEdge::Rising;
		const uint32_t sourceAddress = armed ? source.address : 0;
		const uint32_t preTriggerSamples = armed ? config.trigger.preTriggerSamples : 0;

		/* Three views of the same threshold live in the trigger, and the target
		   compares through the one that matches the declared type of the source. */
		int32_t signedThreshold = 0;
		uint32_t unsignedThreshold = 0;
		float floatThreshold = 0.0f;

		if (armed)
		{
			switch (type)
			{
				case TriggerType::F32:
				{
					floatThreshold = static_cast<float>(config.trigger.value);
					uint32_t bits = 0;
					std::memcpy(&bits, &floatThreshold, sizeof(bits));
					unsignedThreshold = bits;
					signedThreshold = static_cast<int32_t>(bits);
					break;
				}

				case TriggerType::U32:
				{
					/* The comparison on the target is unsigned, so a negative
					   threshold would never be reached and is clamped rather than
					   wrapped into a large positive one. */
					const double nonNegative = std::max(0.0, config.trigger.value);
					const long long rounded = std::llround(std::min(nonNegative, 4294967295.0));
					unsignedThreshold = static_cast<uint32_t>(rounded);
					signedThreshold = static_cast<int32_t>(unsignedThreshold);
					break;
				}

				default:
				{
					signedThreshold = static_cast<int32_t>(config.trigger.value);
					unsignedThreshold = static_cast<uint32_t>(signedThreshold);
					break;
				}
			}
		}

		const uint8_t state = static_cast<uint8_t>(triggerState);

		if (!writeAt(probe, recorderAddress + layout.triggerStateOffset, &state, 1, error))
			return false;

		const uint8_t edge8 = static_cast<uint8_t>(edge);
		const uint8_t type8 = static_cast<uint8_t>(type);

		if (!writeAt(probe, recorderAddress + layout.triggerEdgeOffset, &edge8, 1, error))
			return false;

		if (!writeAt(probe, recorderAddress + layout.triggerTypeOffset, &type8, 1, error))
			return false;

		if (!writeU32(probe, recorderAddress + layout.triggerSourceAddressOffset, sourceAddress, error))
			return false;

		if (!writeU32(probe, recorderAddress + layout.triggerPreTriggerSamplesOffset, preTriggerSamples, error))
			return false;

		if (!writeU32(probe, recorderAddress + layout.triggerValueOffset, static_cast<uint32_t>(signedThreshold), error))
			return false;

		if (!writeU32(probe, recorderAddress + layout.triggerValueU32Offset, unsignedThreshold, error))
			return false;

		const uint32_t floatBits = [&]()
		{
			uint32_t bits = 0;
			std::memcpy(&bits, &floatThreshold, sizeof(bits));
			return bits;
		}();

		if (layout.floatSupport && !writeU32(probe, recorderAddress + layout.triggerValueFloatOffset, floatBits, error))
			return false;

		/* The current and previous values are left alone on purpose. The target
		   copies the current value into the previous one just before its first
		   comparison, so the first sample fires only if it already lies beyond
		   the threshold, and nothing the host writes here can change that. */
		return true;
	}

	bool RecorderHandler::apply(IDebugProbe& probe, const Config& config, std::string& error)
	{
		if (!detected)
		{
			error = "The recorder of this target has not been detected yet. Call detect_recorder first so that the layout of the recorder is known.";
			return false;
		}

		if (config.variables.empty())
		{
			error = "The active recorder group has no variable, so there is nothing to record.";
			return false;
		}

		if (config.variables.size() > detectedSettings.maxVariables)
		{
			error = "The target recorder takes " + std::to_string(detectedSettings.maxVariables) + " variables and the group holds " + std::to_string(config.variables.size()) + ". Raise ____RECORDER_MAXVARS on the target or use fewer plots.";
			return false;
		}

		for (const Slot& slot : config.variables)
		{
			if (slot.size == 0 || slot.size > 4)
			{
				error = "Variable '" + slot.name + "' is " + std::to_string(slot.size) + " bytes wide, the recorder packs one to four.";
				return false;
			}
		}

		const uint32_t packSize = packSizeOf(config.variables);

		if (packSize > detectedSettings.maxBufferSize)
		{
			error = "One sample of the group is " + std::to_string(packSize) + " bytes, the whole buffer of the target is " + std::to_string(detectedSettings.maxBufferSize) + ".";
			return false;
		}

		/* Only whole samples fit, and the target compares the write pointer plus a
		   sample against this value, so a partial sample at the end would let it
		   write past the end of the buffer. */
		const uint32_t maxActualBufferSize = (detectedSettings.maxBufferSize / packSize) * packSize;

		/* The recorder is held stopped while its configuration changes: the target
		   reads these fields from its timer interrupt. */
		if (!writeU32(probe, recorderAddress + layout.stateOffset, static_cast<uint32_t>(State::Init), error))
			return false;

		if (!writeU32(probe, recorderAddress + layout.samplesToSkipOffset, config.skippedSamples, error))
			return false;

		if (!writeU32(probe, recorderAddress + layout.variableCountOffset, static_cast<uint32_t>(config.variables.size()), error))
			return false;

		if (!writeU32(probe, recorderAddress + layout.maxActualBufferSizeOffset, maxActualBufferSize, error))
			return false;

		if (!writeU16(probe, recorderAddress + layout.samplesPackSizeOffset, static_cast<uint16_t>(packSize), error))
			return false;

		if (!writeRingReset(probe, error))
			return false;

		for (size_t index = 0; index < config.variables.size(); index++)
		{
			const uint32_t base = recorderAddress + layout.sampleListOffset + static_cast<uint32_t>(index) * 8;

			if (!writeU32(probe, base, config.variables[index].address, error))
				return false;

			if (!writeU32(probe, base + 4, config.variables[index].size, error))
				return false;
		}

		/* In run mode the trigger stays ready and unarmed, which is what keeps the
		   buffer wrapping instead of stopping at the first pass. Without a trigger
		   in the other two modes the setup state fills the buffer once and stops. */
		TriggerState triggerState = TriggerState::Ready;

		if (config.mode == Mode::Run)
			triggerState = TriggerState::Ready;
		else if (!config.trigger.enabled)
			triggerState = TriggerState::Setup;

		if (!writeTrigger(probe, config, triggerState, error))
			return false;

		return true;
	}

	bool RecorderHandler::start(IDebugProbe& probe, const Config& config, std::string& error)
	{
		if (!apply(probe, config, error))
			return false;

		/* Enabling the sampling comes last: everything the timer interrupt reads
		   has to be in place before it can read it. */
		if (!writeU32(probe, recorderAddress + layout.stateOffset, static_cast<uint32_t>(State::Running), error))
			return false;

		logger->info("Recorder started in {} mode with {} variables, {} bytes per sample, skipping {}",
					 modeToString(config.mode), config.variables.size(), packSizeOf(config.variables), config.skippedSamples);

		running = true;
		return true;
	}

	bool RecorderHandler::stop(IDebugProbe& probe, std::string& error)
	{
		if (!detected)
		{
			error = "The recorder of this target has not been detected yet.";
			return false;
		}

		if (!writeU32(probe, recorderAddress + layout.stateOffset, static_cast<uint32_t>(State::Init), error))
			return false;

		running = false;
		return true;
	}

	bool RecorderHandler::readRingState(IDebugProbe& probe, RingState& out, std::string& error)
	{
		/* Everything up to and including the recorder state sits in the first
		   twenty five bytes, and the ring header is the next eight. */
		uint8_t head[25] = {};

		if (!readAt(probe, recorderAddress, head, sizeof(head), error))
			return false;

		out.samplesToSkip = readU32(head + layout.samplesToSkipOffset);
		out.variableCount = readU32(head + layout.variableCountOffset);
		out.maxActualBufferSize = readU32(head + layout.maxActualBufferSizeOffset);
		out.packSize = readU16(head + layout.samplesPackSizeOffset);
		out.samplesSinceStart = readU32(head + layout.samplesSinceStartOffset);
		out.state = static_cast<State>(head[layout.stateOffset]);

		uint8_t ring[8] = {};

		if (!readAt(probe, recorderAddress + layout.bufferHeadOffset, ring, sizeof(ring), error))
			return false;

		out.head = readU32(ring);
		out.start = readU32(ring + 4);

		return true;
	}

	bool RecorderHandler::download(IDebugProbe& probe, const std::vector<Slot>& variables, Capture& out, std::string& error)
	{
		if (!detected)
		{
			error = "The recorder of this target has not been detected yet. Call detect_recorder first.";
			return false;
		}

		RingState ring;

		if (!readRingState(probe, ring, error))
			return false;

		if (ring.maxActualBufferSize == 0)
		{
			error = "The target recorder has no configuration yet, so there is no buffer to copy. Set the recorder mode first.";
			return false;
		}

		if (ring.packSize == 0 || ring.variableCount != variables.size() || ring.packSize != packSizeOf(variables))
		{
			error = "The buffer was filled with " + std::to_string(ring.variableCount) + " variables of " + std::to_string(ring.packSize) + " bytes, and the group now holds " + std::to_string(variables.size()) + " of " + std::to_string(packSizeOf(variables)) + ". Start the recorder again so that the target packs what the group shows.";
			return false;
		}

		const RingWindow window = ringWindowOf(ring);

		out = Capture{};
		out.ring = ring;
		out.window = window;

		lastDownloadSize = window.bytes;

		if (window.bytes == 0)
		{
			/* Nothing has been stored yet. Not an error: the recorder may have
			   just been started, or the trigger may still be waiting. */
			return decodeRing(ring, variables, std::vector<uint8_t>(0), detectedSettings.timestepNs, ring.samplesToSkip, out, error);
		}

		std::vector<uint8_t> buffer(window.bytes);
		uint32_t offset = 0;

		for (const auto& [start, length] : validRanges(window, ring.maxActualBufferSize))
		{
			if (length == 0)
				continue;

			if (!readAt(probe, recorderAddress + layout.bufferDataOffset + start, buffer.data() + offset, length, error))
				return false;

			offset += length;
		}

		return decodeRing(ring, variables, buffer, detectedSettings.timestepNs, ring.samplesToSkip, out, error);
	}
}  // namespace recorder
