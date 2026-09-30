#ifndef _RecorderHandler_HPP
#define _RecorderHandler_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "IDebugProbe.hpp"
#include "RecorderLayout.hpp"
#include "SerialDriverLayout.hpp"
#include "spdlog/spdlog.h"

/* Declared at global scope on purpose: inside the namespace below it would
   declare recorder::PlotGroup, which is a different type from the one the
   application has. */
class PlotGroup;

/*
 * Host side of the memory recorder.
 *
 * The recorder samples on the target, in a timer interrupt, into a ring buffer
 * that stays in the target memory. The host does not poll anything: it writes
 * the configuration once, lets the target fill the buffer, and copies the buffer
 * out afterwards. That is what makes it possible to record at a rate no debug
 * link could keep up with.
 *
 * The pieces are:
 *
 *   - two globals in the firmware, ____recorderSettings and ____recorder, whose
 *     addresses come from the symbol file the variables come from;
 *   - the configuration, which the host writes into ____recorder before starting
 *     the timer driven sampling;
 *   - a copy of the buffer, moved out with the block read of the probe;
 *   - the reconstruction, which turns the ring state and the bytes into samples.
 *
 * The reconstruction is deliberately free of the probe and of the variable
 * model, so the arithmetic can be checked on its own. Timestamps are seconds
 * from the oldest sample in the buffer. The recorder has no wall clock of its
 * own, only a sample counter, so there is no absolute time to report.
 */

namespace recorder
{
	/* ____RecorderState */
	enum class State : uint8_t
	{
		Init = 0,
		Running = 1,
		Full = 2,
	};

	/* ____TriggerState */
	enum class TriggerState : uint8_t
	{
		Setup = 0,
		Ready = 1,
		Triggered = 2,
		Posttrigger = 3,
	};

	/* ____TriggerType. The numbering is the same as Variable::Type, which is how
	   the trigger source decides its own type. */
	enum class TriggerType : uint8_t
	{
		Unknown = 0,
		U8 = 1,
		I8 = 2,
		U16 = 3,
		I16 = 4,
		U32 = 5,
		I32 = 6,
		F32 = 7,
	};

	/* ____TriggerEdge */
	enum class TriggerEdge : uint8_t
	{
		Rising = 0,
		Falling = 1,
	};

	/* How the host drives the recorder, named after an oscilloscope. The target
	   itself has no notion of re-arming: once the recorder has filled the buffer
	   it stops, so the difference between "single" and "normal" is that the host
	   arms the target again after it has copied the buffer out. */
	enum class Mode
	{
		Run,
		Normal,
		Single,
	};

	std::string modeToString(Mode mode);
	bool modeFromString(const std::string& name, Mode& mode);
	std::string triggerTypeToString(TriggerType type);
	std::string triggerEdgeToString(TriggerEdge edge);

	/* One variable of the recorded set, in the order the target packs it. */
	struct Slot
	{
		uint32_t address = 0;
		uint32_t size = 0;
		/* ____TriggerType of the variable, taken from its declaration. */
		uint8_t type = 0;
		std::string name;
	};

	struct TriggerConfig
	{
		bool enabled = false;
		/* Name of the trigger source. A name rather than a position, because the
		   index a variable sits at depends on which plots a group holds and how
		   many of them are visible, and that changes as the user works. */
		std::string source;
		TriggerEdge edge = TriggerEdge::Rising;
		double value = 0.0;
		uint32_t preTriggerSamples = 0;
	};

	/* What the host writes to the target before a run. */
	struct Config
	{
		Mode mode = Mode::Run;
		uint32_t skippedSamples = 0;
		std::vector<Slot> variables;
		TriggerConfig trigger;
	};

	uint32_t packSizeOf(const std::vector<Slot>& variables);

	/* The ring state as the target holds it. */
	struct RingState
	{
		State state = State::Init;
		uint32_t head = 0;
		uint32_t start = 0;
		uint32_t maxActualBufferSize = 0;
		uint32_t packSize = 0;
		uint32_t samplesSinceStart = 0;
		uint32_t samplesToSkip = 0;
		uint32_t variableCount = 0;
	};

	/* The part of the ring that holds samples, and where the oldest one is. */
	struct RingWindow
	{
		uint32_t oldest = 0;
		uint32_t bytes = 0;
	};

	/* The ring is read as a window of bytes, which is one range unless the write
	   pointer has wrapped, in which case it is two. Offsets are relative to the
	   start of the buffer data, not to the recorder. */
	std::vector<std::pair<uint32_t, uint32_t>> validRanges(const RingWindow& window, uint32_t modulo);

	/* Works out which part of the ring holds samples. The write pointer alone
	   cannot answer this: it reads the same at the moment the buffer becomes
	   full as it does at the moment recording starts, and the two mean opposite
	   things. The sample counter tells them apart. */
	RingWindow ringWindowOf(const RingState& ring);

	struct Capture
	{
		RingState ring;
		RingWindow window;
		uint32_t samples = 0;
		double samplePeriodSeconds = 0.0;
		std::vector<double> timestamps;
		/* Raw values, one vector per variable, in the order of the configuration.
		   Raw and not scaled, because scaling belongs to the variable model. */
		std::vector<std::vector<uint32_t>> series;
	};

	/* Turns the ring state and the bytes into samples. The buffer holds
	   maxActualBufferSize bytes and is indexed modulo that. */
	bool decodeRing(const RingState& ring, const std::vector<Slot>& variables, const std::vector<uint8_t>& buffer,
					uint32_t timestepNs, uint32_t samplesToSkip, Capture& out, std::string& error);

	/* Fills the variable list of a configuration from a recorder group.
	 *
	 * The order matters: the target packs the values in the order it is given
	 * them, and the host unpacks them in the same order, so the two have to be
	 * built from one source. The group is that source. A variable that appears
	 * in two of the group's plots is taken once, because the sample carries each
	 * value once whatever the plots show. */
	bool configFromGroup(const PlotGroup& group, Config& out, std::string& error);

	class RecorderHandler
	{
	   public:
		explicit RecorderHandler(spdlog::logger* logger);

		/* The recorder is a subsystem the user turns on, and the tools that
		   configure it refuse to work while it is off, which is what makes
		   "disabled" mean something more than a hidden window. */
		void setEnabled(bool state) { enabled = state; }
		bool isEnabled() const { return enabled; }

		/* True between the write that enables the sampling and the one that
		   stops it. The tools use it to decide whether a settings change has to
		   be pushed to the target right away. */
		bool isRunning() const { return running; }

		/* The configuration the host will write on the next start. The tools set
		   one field at a time, so it is kept between calls. The variable list is
		   replaced on every start from the group that is active then. */
		const Config& getPendingConfig() const { return pending; }
		void setPendingConfig(const Config& config) { pending = config; }

		/* Addresses of ____recorderSettings and ____recorder, resolved from the
		   same symbol file the variables come from. Changing them drops what was
		   detected, because a layout belongs to one target. */
		void setSymbols(uint32_t settingsAddress, uint32_t recorderAddress);
		void clearSymbols();
		bool hasSymbols() const { return settingsAddress != 0 && recorderAddress != 0; }
		uint32_t getSettingsAddress() const { return settingsAddress; }
		uint32_t getRecorderAddress() const { return recorderAddress; }

		/* Reads ____RecorderSettings and works out the layout. */
		bool detect(IDebugProbe& probe, std::string& error);
		bool isDetected() const { return detected; }
		const Settings& getDetectedSettings() const { return detectedSettings; }
		const Layout& getLayout() const { return layout; }
		void forgetDetection();

		/* The serial driver publishes its own settings the same way the recorder
		   does, and they answer a different question: not what the recorder can
		   hold but how wide a single transfer over the link may be. The driver
		   drops a frame that asks for more entries than it was built with, so a
		   host that assumes a larger number gets nothing back at all. The
		   address comes from the same symbol file; zero means the firmware does
		   not carry the driver, which is the normal state for a hardware probe. */
		void setDriverSymbol(uint32_t settingsAddress);
		bool detectDriver(IDebugProbe& probe, std::string& error);
		bool isDriverDetected() const { return driverDetected; }
		const serial::DriverSettings& getDetectedDriverSettings() const { return driverSettings; }
		void forgetDriverDetection();

		/* Writes the whole configuration and leaves the recorder stopped. */
		bool apply(IDebugProbe& probe, const Config& config, std::string& error);
		/* Writes the configuration and enables the sampling. Calling it again
		   clears the ring and starts a new capture, which is how "normal" and
		   "single" differ from one another on the host side. */
		bool start(IDebugProbe& probe, const Config& config, std::string& error);
		bool stop(IDebugProbe& probe, std::string& error);

		/* Copies the buffer out of the target and rebuilds the samples. */
		bool download(IDebugProbe& probe, const std::vector<Slot>& variables, Capture& out, std::string& error);

		/* Bytes the next download will move, for a progress report. */
		uint32_t getLastDownloadSize() const { return lastDownloadSize; }

	   private:
		bool readRingState(IDebugProbe& probe, RingState& out, std::string& error);
		bool readAt(IDebugProbe& probe, uint32_t address, uint8_t* data, uint32_t size, std::string& error);
		bool writeAt(IDebugProbe& probe, uint32_t address, const uint8_t* data, uint32_t size, std::string& error);
		bool writeU32(IDebugProbe& probe, uint32_t address, uint32_t value, std::string& error);
		bool writeU16(IDebugProbe& probe, uint32_t address, uint16_t value, std::string& error);
		bool writeTrigger(IDebugProbe& probe, const Config& config, TriggerState triggerState, std::string& error);
		bool writeRingReset(IDebugProbe& probe, std::string& error);

		spdlog::logger* logger;

		uint32_t settingsAddress = 0;
		uint32_t recorderAddress = 0;
		uint32_t driverSettingsAddress = 0;
		bool enabled = false;
		bool running = false;
		bool detected = false;
		bool driverDetected = false;
		Settings detectedSettings;
		serial::DriverSettings driverSettings;
		Layout layout;
		Config pending;
		uint32_t lastDownloadSize = 0;
	};
}  // namespace recorder

#endif
