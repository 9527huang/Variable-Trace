#ifndef _RTOSCPULOAD_HPP
#define _RTOSCPULOAD_HPP

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "RtosSnapshot.hpp"

/*
 * Where the time went, from the counters the kernel keeps per task.
 *
 * A run time counter is free running: a timer the application configured
 * increments it, and each control block holds how much of it that task has
 * used. An absolute value says nothing on its own - a share needs the
 * difference between two moments, and a moment to compare against.
 *
 * So this keeps the counters of the last few rounds and answers with each
 * task's part of what all the tasks used across them. The whole is the sum of
 * the tasks rather than the kernel's wall clock counter, which is a variable
 * of the target's own choosing and not something to go looking for by name.
 * The two differ by the time the kernel attributes to no task, interrupts
 * mostly, so what comes out is a share of the time counted for tasks and is
 * named that way rather than as a share of the wall clock.
 *
 * The difference is taken across several rounds rather than the last one
 * because one round is a noisy place to measure a duty cycle: a task that
 * spins for twenty milliseconds every thirty will read anywhere between a
 * tenth and half of the time depending on whether it happened to be preempted
 * inside that one window. Several rounds average that out, and the number
 * stops moving about under the reader's eyes.
 *
 * The reference is one round for every task together, the oldest one still
 * held rather than the oldest that has each task: a share is a fraction of one
 * window, and a fraction whose numerator and denominator cover different
 * windows is not a fraction of anything. A task that was not listed in the
 * reference round goes without a share until the reference slides past the
 * round it was missing from.
 *
 * No clock is read here. The caller passes the time each round in the units
 * the snapshot uses, which keeps this testable against a made up clock.
 */
class RtosCpuLoad
{
   public:
	struct Settings
	{
		/* How many rounds a share is measured over. The window in seconds is
		   this times the round interval, and is reported beside the shares
		   rather than assumed. */
		uint32_t windowRounds = 4;
	};

	/*
	 * Works out each task's share of what the tasks used since the reference
	 * round, writes it into the tasks, fills in the summary, and keeps this
	 * round's counters to be the reference for later ones.
	 *
	 * `summary.supported` is left alone: whether the target keeps such a
	 * counter at all is a fact about the build, known to whoever read the
	 * control block, and not something samples can tell.
	 */
	void step(std::vector<RtosTask>& tasks, RtosCpuLoadSummary& summary, const Settings& settings, double nowSeconds);

	/* Drops the reference counters: used when the target under the view
	   changes, so that a difference is never taken across two kernels. */
	void forget();

	size_t rememberedRounds() const
	{
		return rounds.size();
	}

   private:
	struct Sample
	{
		uint32_t counter = 0;

		/* The kernel's own task number, where it keeps one. It survives a
		   task being deleted and another taking its control block, which is
		   the one thing the address cannot tell apart. */
		uint32_t kernelTaskNumber = 0;
		bool hasKernelTaskNumber = false;
	};

	/* One round's counters, and when it was taken. */
	struct RoundCounters
	{
		std::map<uint32_t, Sample> counters;
		double takenAtSeconds = 0.0;
	};

	std::deque<RoundCounters> rounds;

	/*
	 * What each address was last seen to use, in counts per second.
	 *
	 * A round regularly comes back without a few of its tasks - the delayed
	 * list is rewritten every tick, and a walk of it often stops part way. A
	 * task that cannot be read cannot be measured, but it can still be counted
	 * into the whole at what it was last seen to use, and leaving it out
	 * instead is what turns a share into a wrong answer: on the fixture target
	 * a round that missed half its tasks reported the busiest of the rest at
	 * 92 per cent when its ceiling is about 40. The rates are steady enough
	 * for this to be a good estimate, and how many tasks it was used for
	 * travels with the snapshot so the reader knows.
	 */
	std::map<uint32_t, double> lastRate;
};

#endif
