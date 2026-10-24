#ifndef _RTOSTRACEPLAYBACKV3_HPP
#define _RTOSTRACEPLAYBACKV3_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "RtosRecording.hpp"
#include "RtosRecordingV3.hpp"

/*
 * A v3 recording, read as a run of rounds.
 *
 * The container knows about blocks; the interface knows about rounds - how many,
 * what time each one is at, and what was in it. This is the one place the two
 * meet, so that the file format can grow a block type without every caller
 * learning what a block is.
 *
 * A round is decoded when it is asked for, not when the file is opened: a v3
 * file can hold hours of them, and opening a recording should not depend on how
 * long it is. What opening does do is find every `SnapshotRound` block, which is
 * a walk over the block headers only - the payloads stay on disk.
 *
 * The same class will be what the model uses once the recorder writes v3. Until
 * then it is the read half of a format nobody writes yet, which is exactly why
 * it is tested against a file produced by the old writer and converted.
 */
class RtosPlaybackV3
{
   public:
	struct Round
	{
		std::shared_ptr<RtosSnapshot> snapshot;
		std::vector<RtosEventRing::Interval> events;
	};

	RtosPlaybackV3() = default;

	RtosPlaybackV3(const RtosPlaybackV3&) = delete;
	RtosPlaybackV3& operator=(const RtosPlaybackV3&) = delete;

	/* Opens and indexes the round blocks. A file with no round block at all is
	   refused: a container with only diagnostics in it is not something the
	   interface can show. */
	bool open(const std::string& path, RtosRecording::Problem& problem);
	void close();

	bool isOpen() const
	{
		return opened;
	}

	size_t roundCount() const
	{
		return rounds.size();
	}

	/* The time of a round.
	 *
	 * Taken at open time from the first eight bytes of the round - the encoding
	 * writes `captureStartedAt` first, so the number the interface scrolls by
	 * costs nothing and the rest of the payload stays on disk until asked for. */
	double atSeconds(size_t round) const;

	std::shared_ptr<RtosSnapshot> roundAt(size_t round) const;
	Round roundWithEvents(size_t round) const;

	/* Whether this file carries switch records. A v3 file always does: the
	   encoder writes them as part of the round, and a v3 file without any means
	   the target never switched rather than that nobody wrote them down. */
	bool hasEventRecords() const
	{
		return opened;
	}

	/*
	 * The events, the identities their keys point at, and the source.
	 *
	 * Empty for a file written before the event blocks existed - the rounds and
	 * their runs are all it has. `hasTraceEvents` is what a reader checks before
	 * drawing bands: an empty history that means "not written down" must not be
	 * drawn as a session with nothing in it.
	 */
	bool hasTraceEvents() const
	{
		return haveEvents;
	}

	const std::vector<RtosNormalizedEvent>& events() const
	{
		return traceEvents;
	}

	const RtosIdentityStore& identities() const
	{
		return traceIdentities;
	}

	const RtosTraceSourceRecord& source() const
	{
		return sourceRecord;
	}

	const std::string& path() const
	{
		return filePath;
	}

	uint64_t bytes() const
	{
		return fileBytes;
	}

	/* The block types the file holds, for the reader who wants to know what
	   else is in there besides rounds. */
	const std::vector<RtosRecordingV3::BlockType>& blockTypes() const
	{
		return types;
	}

   private:
	struct RoundRef
	{
		/* Which block, and which round inside it. */
		size_t block = 0;
		size_t index = 0;
		double atSeconds = 0.0;
	};

	/* Decodes one round and remembers when it happened. Returns false when the
	   round does not decode, which callers treat as a round that is not there
	   rather than as a broken file: one bad round should not cost the others. */
	bool decode(const RoundRef& reference, Round& out) const;

	std::vector<std::vector<std::vector<uint8_t>>> blockRounds;  /* per block, its encoded rounds */
	std::vector<RoundRef> rounds;
	std::vector<RtosRecordingV3::BlockType> types;

	/* Decoded as the blocks are walked, in file order. The dictionary is
	   replaced rather than merged when a second one appears: the writer appends
	   a fresher copy, and the last one is the one the events were written
	   against. */
	std::vector<RtosNormalizedEvent> traceEvents;
	RtosIdentityStore traceIdentities;
	RtosTraceSourceRecord sourceRecord;
	bool haveEvents = false;

	std::string filePath;
	uint64_t fileBytes = 0;
	bool opened = false;
};

#endif
