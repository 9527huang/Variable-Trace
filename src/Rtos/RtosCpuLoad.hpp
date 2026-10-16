#ifndef _RTOSCPULOAD_HPP
#define _RTOSCPULOAD_HPP

#include <cstdint>
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
 * So this keeps the counters of the previous round and answers with each
 * task's part of what all the tasks used in between. The whole is the sum of
 * the tasks rather than the kernel's wall clock counter, which is a variable
 * of the target's own choosing and not something to go looking for by name.
 * The two differ by the time the kernel attributes to no task, interrupts
 * mostly, so what comes out is a share of the time counted for tasks and is
 * named that way rather than as a share of the wall clock.
 *
 * Two rounds have to be read before there is an answer, and both have to hold
 * the same task. A task missing from either round goes without a share that
 * round: a fraction whose numerator and denominator cover different windows
 * is not a fraction of anything.
 *
 * No clock is read here. The caller passes the time each round in the units
 * the snapshot uses, which keeps this testable against a made up clock.
 */
class RtosCpuLoad
{
   public:
	/*
	 * Works out each task's share of what the tasks used since the previous
	 * round, writes it into the tasks, fills in the summary, and keeps this
	 * round's counters to answer the next one from.
	 *
	 * `summary.supported` is left alone: whether the target keeps such a
	 * counter at all is a fact about the build, known to whoever read the
	 * control block, and not something two rounds of samples can tell.
	 */
	void step(std::vector<RtosTask>& tasks, RtosCpuLoadSummary& summary, double nowSeconds);

	/* Drops the reference counters: used when the target under the view
	   changes, so that a difference is never taken across two kernels. */
	void forget();

	size_t rememberedCount() const
	{
		return previous.size();
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

		double takenAtSeconds = 0.0;
	};

	std::map<uint32_t, Sample> previous;
};

#endif
