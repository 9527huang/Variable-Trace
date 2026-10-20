#ifndef _RTOSTIMELINE_HPP
#define _RTOSTIMELINE_HPP

#include <cstdint>
#include <mutex>
#include <vector>

#include "RtosSnapshot.hpp"

/*
 * What the task view saw, kept for the timeline.
 *
 * A round is a picture of one instant and is replaced by the next one. A
 * timeline is the same information over time, so every published round leaves
 * a row behind: what each task was doing at the moment the round was taken.
 *
 * What is kept is the sample, not the interval. Two rounds half a second apart
 * say what was true at two moments and nothing about the half second between
 * them, and drawing that span as a band would claim a task ran for half a
 * second when all that was seen is that it was running at an instant inside
 * it. So the view is given samples and draws samples, and the spacing between
 * them is the round interval - which the round itself reports, so nothing here
 * has to assume it.
 *
 * A ring, because the interesting window is the last few minutes while a
 * debugging session runs for hours. The oldest samples are dropped and the
 * view can say how far back it reaches.
 *
 * Read from the interface thread and written from the sampling thread, so the
 * two are kept apart by a lock rather than by a copy of everything: the reader
 * asks for the range it is about to draw, which is a few hundred samples, and
 * gets its own copy of those.
 */
class RtosTimeline
{
   public:
	struct Entry
	{
		uint32_t tcbAddress = 0;
		RtosTaskState state = RtosTaskState::Unknown;

		/* The share the round gave this task, when it gave one. A sample with
		   no share is a task the round could not measure, which is not the
		   same as a task that used no time. */
		bool hasCpuShare = false;
		double cpuSharePercent = 0.0;
	};

	struct Sample
	{
		double atSeconds = 0.0;
		std::vector<Entry> entries;
	};

	/*
	 * One task holding one state from one moment to another.
	 *
	 * The strip used to draw a mark per sample, which reads as a row of dots:
	 * a dot says "at this instant it was ready", and the time between two dots
	 * was left blank. Blank is what the eye reads as "nothing happened", so a
	 * task waiting for two seconds looked like a task that was not there.
	 *
	 * An interval says the same thing with a length: from `from` to `to`, this
	 * task was in this state. Adjacent samples that agree become one interval,
	 * so a lane is a continuous band of colour rather than a dotted line, and
	 * the one thing a picture is good at - showing how much - is available.
	 */
	struct StateInterval
	{
		uint32_t tcbAddress = 0;
		RtosTaskState state = RtosTaskState::Unknown;
		double from = 0.0;
		double to = 0.0;
	};

	/*
	 * Turns samples into intervals, one task at a time.
	 *
	 * A sample stands for the time from itself until the next sample: the round
	 * saw the task in that state, and nothing said otherwise until the next
	 * look. The last sample is given no length of its own - there is no next
	 * look to bound it - so callers pass the window end through `untilSeconds`
	 * when they want the band to reach the right edge.
	 *
	 * A task missing from a round ends its interval there. Missing is not a
	 * state: it means the round did not read that task, and the strip has a
	 * separate way to say so.
	 */
	static std::vector<StateInterval> stateIntervals(const std::vector<Sample>& samples, double untilSeconds);

	/* How many rounds are remembered. At the round interval the task view
	   runs at, the default is about half an hour. */
	static constexpr size_t defaultCapacity = 4096;

	struct Span
	{
		double from = 0.0;
		double to = 0.0;
	};

	/*
	 * What the strip shows, given the cursors the variable view has placed.
	 *
	 * Two cursors make the window the span between them - the measurement the
	 * reader drew - and one centres a window on itself. Following them matters
	 * because a cursor is placed at the moment being looked at, and that moment
	 * is usually well behind the newest round: a strip pinned to the recent past
	 * would show no line at all, and sharing the cursor would then be a promise
	 * the screen does not keep.
	 *
	 * With no cursors, or with following switched off, the window is the last
	 * `windowSeconds` of what was kept.
	 */
	static Span windowFrom(const std::vector<double>& cursors, double windowSeconds, bool follow, double newest);

	explicit RtosTimeline(size_t capacity = defaultCapacity);

	/* Keeps one round. The oldest sample is dropped once the ring is full. */
	void add(const RtosSnapshot& snapshot);

	/* Forgets everything: a new session is a new history, and two runs drawn
	   on one axis would be a picture of neither. */
	void clear();

	/*
	 * Copies the samples in the closed range into `into`, oldest first.
	 *
	 * A copy rather than a view, because the ring is being written to while
	 * the interface reads it. The caller passes the range it is about to draw,
	 * so the copy is proportional to what is on screen and not to how long the
	 * session has been running.
	 */
	void copyRange(double fromSeconds, double toSeconds, std::vector<Sample>& into) const;

	/*
	 * Which of `samples` was taken closest to `atSeconds`.
	 *
	 * A round is an instant and a cursor falls between them, so reading one out
	 * means naming the nearest - and saying it. Returns samples.size() when
	 * there are none.
	 */
	static size_t nearestIndex(const std::vector<Sample>& samples, double atSeconds);

	size_t size() const;

	/* The ends of what is held, in the same seconds the snapshot uses. Both
	   are zero when nothing has been kept. */
	double oldestSeconds() const;
	double newestSeconds() const;

   private:
	mutable std::mutex mtx;
	std::vector<Sample> samples;
	size_t next = 0;
	size_t filled = 0;
	size_t capacity = 0;
};

#endif
