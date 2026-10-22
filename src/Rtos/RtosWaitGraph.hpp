#ifndef _RTOSWAITGRAPH_HPP
#define _RTOSWAITGRAPH_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "RtosSnapshot.hpp"

/*
 * Who is blocking whom, as a chain rather than a sentence.
 *
 * A task that is blocked is waiting for an object, and the object is held by
 * another task, which may itself be waiting. Written out this is a sentence
 * with names in it; drawn it is a few boxes and arrows, and the shape is what
 * tells a reader whether they are looking at a slow path or a ring that will
 * never open. `RtosDiagnostics` already finds those rings, and its findings
 * carry the same chain as names - this turns the same relation into something
 * a reader can see, for every blocked task rather than only the diagnosed.
 *
 * Only one direction is followed: what the task is waiting for, and who holds
 * it. "Who is waiting on me" is the other half of the same picture and would
 * turn one chain into a tree with a task at every fork; the count of waiters is
 * on the object's row instead.
 *
 * Nothing here draws or reads a target. It is arithmetic over one snapshot,
 * which is why the rules - the stop conditions, the ring - can be held to a
 * test.
 */
class RtosWaitGraph
{
   public:
	enum class NodeKind
	{
		Task,
		Object,
	};

	struct Node
	{
		NodeKind kind = NodeKind::Task;
		uint32_t address = 0;
		std::string name;

		/* Tasks: the state they are in and their priority. Objects: what kind
		   of object it is and how many of it are taken. Both are shown inside
		   the box, so that the chain does not need a table beside it. */
		RtosTaskState state = RtosTaskState::Unknown;
		bool hasState = false;
		uint32_t priority = 0;
		bool hasPriority = false;

		RtosSyncKind syncKind = RtosSyncKind::Unknown;
		uint32_t capacity = 0;
		uint32_t items = 0;
		bool hasCounts = false;

		/* How many tasks are waiting on this object, and whether that count was
		   readable. The chain follows one of them - the holder's - so the
		   others would otherwise vanish from the picture. */
		uint32_t waitingOnThis = 0;
		bool hasWaiters = false;
	};

	struct Step
	{
		size_t from = 0;
		size_t to = 0;

		/* True for the step from an object to the task holding it, false for
		   the step from a task to the object it waits for. */
		bool heldBy = false;

		/* Set when this step arrives back at a node already in the chain: the
		   ring is closed, and the last box is the first one again rather than a
		   fifth copy of it. */
		bool closesCycle = false;
	};

	struct Chain
	{
		/* `nodes[0]` is the task that was asked about, whether or not it is
		   waiting on anything. */
		std::vector<Node> nodes;
		std::vector<Step> steps;
		bool cycle = false;
	};

	/*
	 * The chain starting at `task`.
	 *
	 * `limit` bounds how many boxes are built, counted in nodes. A chain that
	 * takes more than that to close is cut rather than drawn wider than the
	 * block it goes in; the cut is visible because no ring is marked.
	 */
	static Chain from(const RtosSnapshot& snapshot, uint32_t task, size_t limit = 6);
};

#endif
