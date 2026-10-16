#ifndef _RTOSLISTWALKER_HPP
#define _RTOSLISTWALKER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "RtosMemory.hpp"
#include "RtosProfileBase.hpp"

/*
 * Walking one kernel list.
 *
 * A FreeRTOS list is a circular doubly linked list whose sentinel lives inside
 * the `List_t` itself. Walking it chases a pointer per item, so the cost is one
 * read for the head and one per item - which is exactly what the probe budget
 * in the plan was computed from. Nothing here reads a whole array at once
 * because there is no array: the items live in the control blocks.
 *
 * The walk is written to survive a target running underneath it. Each item is
 * read whole, so the backward pointer can be checked against where the walk
 * actually came from, and four separate failures - a read that failed, a field
 * the profile could not resolve, a chain that folds back on itself, and a chain
 * that disagrees with the count the list declares - come back as four separate
 * answers rather than as one "it did not work".
 */

/*
 * How far a walk goes before deciding the chain is not a chain.
 *
 * More tasks at a single priority than this is possible in theory and unheard
 * of in practice. The limit is here so that a damaged pointer cannot turn one
 * snapshot into an endless walk.
 *
 * It sits outside the class because a nested type cannot be the type of a
 * default argument of a member function of the class that nests it.
 */
struct RtosWalkLimits
{
	uint32_t maxItems = 4096;
};

class RtosListWalker
{
   public:
	struct Item
	{
		uint32_t address = 0;
		uint32_t value = 0;
		uint32_t next = 0;
		uint32_t previous = 0;
		uint32_t owner = 0;
		uint32_t container = 0;
	};

	enum class Result
	{
		Ok,
		/* A read did not come back. */
		ReadingFailed,
		/* The profile has no offset for a field the walk needs. */
		LayoutIncomplete,
		/* The chain came back to a node already visited. */
		Loop,
		/* The chain is longer than the limit allows. */
		TooManyItems,
		/* A node's backward pointer does not point at the node before it, so
		   what the walk is following is not the chain it started on. */
		LinksDisagree,
	};

	struct Walk
	{
		Result result = Result::Ok;
		uint32_t listAddress = 0;
		uint32_t declaredCount = 0;
		std::vector<Item> items;

		/* Set when the walk finished but the item count the list declares did
		   not match what was found. The items are still handed back, because
		   the caller decides what a disagreement does to the round. */
		bool countDisagrees = false;

		bool ok() const
		{
			return result == Result::Ok;
		}
	};

	static Walk walk(IRtosMemory& memory, const RtosProfileBase::Resolution& layout, uint32_t listAddress, const RtosWalkLimits& limits = {});

	/* The sentence the interface and the log show for a walk that stopped. */
	static std::string describe(Result result);
};

#endif
