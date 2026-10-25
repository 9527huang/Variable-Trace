#ifndef _RTEC2SWOSOURCE_HPP
#define _RTEC2SWOSOURCE_HPP

#include <cstdint>
#include <deque>
#include <vector>

#include "IRtosTraceSource.hpp"
#include "Rtec2Decoder.hpp"
#include "Rtec2Frames.hpp"
#include "RtosTraceTypes.hpp"

/*
 * The target's records, arriving as a byte stream off the trace unit.
 *
 * The ring has to be read between rounds, and what the writer overwrote in the
 * meantime is gone. A stream has no such gap - every record goes out in order -
 * which is why it is worth a wire and the trace registers.
 *
 * What this adds over the framing reader alone is the two things a stream has
 * to say for itself because there is no control block beside it:
 *
 * - **The clock's unit.** A record carries ticks and nothing that says what
 *   they are. `ClockSync` says it, and the target repeats it once a second so
 *   that a host which connected after boot is not left converting ticks by
 *   guesswork.
 * - **Its own state.** A stream is late rather than lost: nothing tells the
 *   host how many records it missed, only that the bytes stopped making
 *   frames. `Stats` counts what was thrown away, and the panel can say so.
 *
 * Bytes are handed over as the probe gives them, in any sizes: a frame split
 * across two reads is the normal case at a few hundred bytes a read.
 */
class Rtec2SwoSource final : public IRtosTraceSource
{
   public:
	void feed(const uint8_t* bytes, size_t size);
	void feed(const std::vector<uint8_t>& bytes)
	{
		feed(bytes.data(), bytes.size());
	}

	bool start(RtosTraceProblem& problem) override;
	void stop() override;
	void requestNewEpoch() override;

	/* What the target says it can do. A stream carries no capability record the
	   host can rely on finding (the one at boot is written before anybody is
	   listening), so this starts empty and grows as records arrive. The panel
	   reads it as "not carried" until then, which is the honest answer for the
	   first second of a session. */
	RtosTraceCapabilities capabilities() const override;

	RtosTraceSourceState state() const override;

	const char* name() const override
	{
		return "rtec2-swo";
	}

	RtosTraceBatch poll(double budgetSeconds) override;

	struct Stats
	{
		uint32_t frames = 0;
		uint32_t records = 0;
		uint32_t badFrames = 0;
		uint64_t resyncBytes = 0;
		uint32_t unknownTypes = 0;
		uint32_t targetHz = 0;
	};

	const Stats& stats() const
	{
		return counters;
	}

	/* Bytes held that have not made a frame. A number that keeps growing is a
	   wire that is running and a reader that is not. */
	size_t pendingBytes() const
	{
		return frames.pendingBytes();
	}

   private:
	Rtec2FrameReader frames;

	std::deque<Rtec2DecodedRecord> pending;

	Stats counters;

	uint64_t cursor = 0;
	uint64_t epoch = 1;
	RtosTraceCapabilities caps;

	RtosTraceSourceState sourceState = RtosTraceSourceState::NotStarted;
	bool newEpochWanted = false;
};

#endif
