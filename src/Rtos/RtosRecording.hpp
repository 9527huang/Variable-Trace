#ifndef _RTOSRECORDING_HPP
#define _RTOSRECORDING_HPP

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "RtosEventRing.hpp"
#include "RtosSnapshot.hpp"
#include "Trace/RtosIdentityStore.hpp"
#include "Trace/RtosTracePipeline.hpp"
#include "Trace/RtosTraceTypes.hpp"

class RtosPlaybackV3;
class RtosRecordingV3;

/*
 * The task view, written down.
 *
 * A session with a probe is the only time the task view has data, and it is
 * the worst time to study it: the board is on the bench, the machine that owns
 * it is usually somewhere else, and the interesting round - the one where two
 * tasks deadlocked - has already been overwritten. What this file holds is
 * every round that was published, in order, so that the whole view (the table,
 * the findings, the lanes, the modes) can be worked through later, on a
 * machine with no probe attached at all.
 *
 * A round is a snapshot, so what is written is the snapshot: the file is a
 * record of the data the view already had, not a second model of the target.
 * Fields are written one by one rather than as a memory image, so the format
 * belongs to this file and not to the layout of a struct that will keep
 * growing.
 *
 * Each record stands on its own - names included, not kept in a table shared
 * with the others. A table would be smaller, and it would mean reading the
 * whole file to decode any of it; a reader that jumps to one round is worth
 * more than the third of the file a table saves.
 *
 * Two roles, never at once: writing (from the sampling thread, while a target
 * is being read) and reading (from the interface thread, with no target).
 */
class RtosRecording
{
   public:
	/*
	 * Two containers, one reader.
	 *
	 * A version 3 file is a container of blocks; a version 2 file is a run of
	 * records. Everything above this class - the panel, the export, the tools -
	 * only ever asks for "how many rounds, and give me that one", so the format
	 * is decided here and nowhere else. `open` sniffs the file and remembers
	 * which reader answers.
	 */

	/* Bumped when the fields below change. A file from another version is
	   refused with a sentence rather than read as far as it happens to make
	   sense: half a round decoded as a whole one is worse than no file.
	   2: each round carries the switch records that arrived with it. A file
	   from version 1 still opens - it simply has none, and the event list says
	   so instead of claiming the target never switched. */
	static constexpr uint32_t formatVersion = 2;

	/* The oldest layout this reader understands. */
	static constexpr uint32_t oldestReadableVersion = 1;

	/*
	 * Why an attempt to open or start failed.
	 *
	 * A code rather than a sentence: the words belong to the interface, which
	 * is the only place that knows what language the reader has chosen. The
	 * English text below is for the log.
	 */
	enum class Problem
	{
		None,
		CannotCreate,
		CannotOpen,
		NotARecording,
		WrittenByAnotherVersion,
		NoRounds,
		AnotherRecordingIsOpen,
		ARecordingIsBeingWritten,
	};

	static const char* describe(Problem problem)
	{
		switch (problem)
		{
			case Problem::CannotCreate:
				return "the file could not be created";

			case Problem::CannotOpen:
				return "the file could not be opened";

			case Problem::NotARecording:
				return "this is not a task view recording";

			case Problem::WrittenByAnotherVersion:
				return "this recording was written by another version of the program";

			case Problem::NoRounds:
				return "this recording holds no rounds";

			case Problem::AnotherRecordingIsOpen:
				return "a recording is already open for reading";

			case Problem::ARecordingIsBeingWritten:
				return "a recording is being written";

			case Problem::None:
			default:
				return "";
		}
	}

	/* What a reader wants to know before deciding to open anything. */
	struct Info
	{
		size_t rounds = 0;
		double firstSecond = 0.0;
		double lastSecond = 0.0;
		uint64_t bytes = 0;

		double durationSeconds() const
		{
			return lastSecond - firstSecond;
		}
	};

	/* Both out of line: the v3 reader is only forward declared here, so the
	   construction and destruction of the pointer to it have to happen where
	   the type is complete. */
	RtosRecording();
	~RtosRecording();

	RtosRecording(const RtosRecording&) = delete;
	RtosRecording& operator=(const RtosRecording&) = delete;

	/*
	 * Writing.
	 *
	 * `write` is called from the thread that publishes rounds. It takes the
	 * file for as long as it takes to append one record; the interface only
	 * reads the counters, which is why they are updated in one step under the
	 * same lock.
	 */
	/* Which container a recording is written in.
	 *
	 * Two exist because the second can hold more than rounds; a recording is
	 * still a run of rounds either way, so this is the only choice the caller
	 * makes and the only place the two differ when writing. Reading decides for
	 * itself by looking at the file. */
	enum class Format
	{
		V2,
		V3,
	};

	bool begin(const std::string& path, Problem& problem, Format format = Format::V2);
	/*
	 * One round, and the switch records that arrived with it.
	 *
	 * The runs travel with the round that saw them rather than in a section of
	 * their own: the file is read one round at a time, and a reader that wants
	 * a round's lanes should not have to walk an index twice. They are also the
	 * same runs the live view merges at batch seams, so a file read back
	 * through the same merge gives the same lanes.
	 */
	void write(const RtosSnapshot& snapshot, const std::vector<RtosEventRing::Interval>& events = {});

	/*
	 * The events themselves, for the blocks a version 3 container has.
	 *
	 * A round carries the switch runs because the lanes are drawn from them; the
	 * tracks, the time budget and the fault list are answers about events, and a
	 * file without them reopens as a table with empty bands. The runs and the
	 * events are not duplicates: a run is a stretch of time, an event is a
	 * transition, and only the second one can say who was interrupted.
	 *
	 * Ignored for a version 2 file, which has nowhere to put them. `source` is
	 * written once per recording, beside the first batch.
	 */
	void writeTraceEvents(const std::vector<RtosNormalizedEvent>& events, const RtosTraceSourceRecord& source);

	/* Who the keys in those events belong to. Written every so often and once
	   more when the recording stops, so that a file cut short still has a
	   dictionary to read its events against. */
	void writeIdentities(const RtosIdentityStore& identities);

	void finish();

	bool isWriting() const
	{
		return writing;
	}

	/* Where it is being written, for the line on screen. */
	const std::string& path() const
	{
		return filePath;
	}

	Info stats() const;

	/*
	 * Reading.
	 *
	 * Opening reads the header and walks the record headers once to learn where
	 * each round starts and what second it was taken at - eight bytes a round,
	 * not a decode. A round is then read on demand, so a reader keeps one
	 * snapshot at a time instead of the whole recording.
	 */
	bool open(const std::string& path, Problem& problem);
	void close();

	bool isOpen() const
	{
		return reading;
	}

	size_t roundCount() const;

	/* The moment a round was taken, on the same clock the live view uses. */
	double atSeconds(size_t round) const;

	/*
	 * Which round was taken closest to `atSeconds`, for a click on the strip.
	 * Returns the count when there are no rounds.
	 */
	size_t nearestRound(double atSeconds) const;

	/* The round, decoded. Null when the round is out of range or the file has
	   stopped making sense - a truncated file is not a reason to take the
	   application down with it. */
	std::shared_ptr<RtosSnapshot> roundAt(size_t round) const;

	/* A round together with the runs that came with it, decoded in one pass.
	   Opening a file wants both and doing them separately would decode the
	   same record twice. */
	struct Round
	{
		std::shared_ptr<RtosSnapshot> snapshot;
		std::vector<RtosEventRing::Interval> events;
	};

	Round roundWithEvents(size_t round) const;

	/*
	 * The bytes of one round, without the v2 record header.
	 *
	 * Public because the newer container stores the same encoding inside its
	 * blocks: one definition of what a round looks like, so that a file
	 * written by either path decodes the same way. `withEvents` says whether
	 * the tail of switch records is present, which is what the file version
	 * used to decide.
	 */
	static void encodeRound(const RtosSnapshot& snapshot, const std::vector<RtosEventRing::Interval>& events, std::vector<uint8_t>& out);
	static bool decodeRound(const uint8_t* bytes, size_t size, bool withEvents, Round& out);

	/* Whether this file holds switch records at all. A file written before
	   they were kept opens and looks complete; only this tells the reader that
	   an empty event list means "not recorded" rather than "nothing switched". */
	bool hasEventRecords() const;

	/*
	 * What a version 3 file holds besides the rounds.
	 *
	 * `present` is false for v1 and v2, which kept the runs and not the events.
	 * The interface uses it to say "this recording was made before events were
	 * written down" rather than showing empty tracks as if the target had
	 * nothing to report.
	 */
	struct TraceContent
	{
		bool present = false;
		std::vector<RtosNormalizedEvent> events;
		RtosIdentityStore identities;
		RtosTraceSourceRecord source;
	};

	TraceContent traceContent() const;

	/* Whether the open file carries events. Cheap - it asks the container, not
	   the events - because the interface asks it every frame. */
	bool hasTraceEvents() const;

	Info info() const;

   private:
	struct IndexEntry
	{
		uint64_t offset = 0;
		double atSeconds = 0.0;
		uint32_t payloadBytes = 0;
	};

	std::vector<IndexEntry> index;

	/* Writing state, guarded: the file is appended from the sampling thread
	   while the interface reads the counters. */
	mutable std::mutex mtx;
	std::FILE* file = nullptr;

	bool writing = false;
	bool reading = false;

	/* Set when the open file is a version 3 container. Null for v1/v2 and while
	   writing: only one of the two readers is ever live. */
	std::unique_ptr<RtosPlaybackV3> v3;

	/* Set while a version 3 recording is being written. Never at the same time as
	   the reader above: a recording and a playback are exclusive. */
	std::unique_ptr<RtosRecordingV3> v3Writer;

	/* The v1/v2 reader's own teardown, split out so that `close` can free
	   whichever of the two readers is live without repeating itself. */
	void closeV2();

	std::string filePath;

	/* The version the open file declares, which decides whether a record has an
	   events block at its end. */
	uint32_t fileVersion = formatVersion;

	uint64_t roundsWritten = 0;
	double firstWrittenSecond = 0.0;
	double lastWrittenSecond = 0.0;
	bool haveFirstSecond = false;

	/* One record's bytes, reused: a record is assembled and then written in
	   one call, so a reader never sees half of one. */
	std::vector<uint8_t> buffer;

	/* Whether the source descriptor has been written into this recording. One
	   per file: it describes the session, not each batch. */
	bool sourceWritten = false;
};

#endif
