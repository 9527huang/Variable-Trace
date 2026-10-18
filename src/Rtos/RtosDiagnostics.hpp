#ifndef _RTOSDIAGNOSTICS_HPP
#define _RTOSDIAGNOSTICS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "RtosSnapshot.hpp"
#include "RtosTimeline.hpp"

/*
 * What one round says is wrong, in words and in tasks.
 *
 * Two findings come out of a single snapshot without guessing.
 *
 * A task whose current priority is above its base one is having its priority
 * lent to it: the kernel only does that when a higher priority task is waiting
 * for a lock the task holds, so the pair is a lock held too long, and the task
 * in the middle is the one that took it.
 *
 * A cycle in the holds-and-waits graph is a deadlock. "Suspected" is the honest
 * word for it: a round is a sample, and a cycle that resolves between two
 * rounds leaves no trace in the second. What is reported is that this round saw
 * one - which for a real deadlock is every round from then on.
 *
 * Nothing here reads the target. It is arithmetic over what the round already
 * collected, which is why it can be held to tests.
 */
class RtosDiagnostics
{
   public:
	enum class Kind
	{
		PriorityInheritance,
		SuspectedDeadlock,

		/* A task higher in priority than the one the scheduler was running, and
		   ready in every round of the window. Not a matter of opinion: with a
		   preemptive priority scheduler, that cannot happen - except while the
		   running task is holding a lock someone above it wants. */
		ReadyStarvation,
	};

	struct Finding
	{
		Kind kind = Kind::PriorityInheritance;

		/* The names as the round gave them, in the order the ring goes: task,
		   object, task, object, ... A task that could not be named is its
		   address, so the chain never has a hole in it. */
		std::vector<std::string> chain;

		/* The tasks involved, in the same order, for selecting them in the
		   table. Zero for a name that is not a task. */
		std::vector<uint32_t> tasks;

		/* For a starvation: how long the task has been ready without being
		   switched in, and how many rounds were read to say so. */
		double readySeconds = 0.0;
		uint32_t heldRounds = 0;
	};

	/* `history` is the sampled rounds. A starvation is the one finding that
	   needs more than the round in front of it: one round saying a high
	   priority task is ready while a lower one runs is a moment, and a moment
	   is not a fault - a switch may be about to happen. Four rounds of it is a
	   fault. */
	static std::vector<Finding> diagnose(const RtosSnapshot& snapshot, const RtosTimeline& history);
};

#endif
