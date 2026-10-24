#ifndef _RTOSIDENTITYSTORE_HPP
#define _RTOSIDENTITYSTORE_HPP

#include <cstdint>
#include <map>
#include <string>

#include "RtosTraceTypes.hpp"

/*
 * Who is who, across lifetimes of the same address.
 *
 * A TCB address is reused. Between two uses of 0x20001B60 there were two tasks,
 * and a history that keys on the address alone will join them: the second
 * task's first switch appears to be the first task waking up, the CPU column
 * counts the wrong one, and the waiting chain draws a deadlock that never
 * existed. Generations exist to stop exactly that.
 *
 * The store is the only place a generation is decided. It does three things:
 *
 * - **Opens and closes lifetimes** on `TaskCreate` / `TaskDelete` (and the sync
 *   equivalents), numbering them per handle from one.
 * - **Keeps what it saw** when the stream joins mid-session: an event for a
 *   handle nobody created becomes an entity whose generation is unknown and
 *   whose origin is marked. It is usable for display and cannot be trusted as
 *   an identity - and the mark says which.
 * - **Forgets nothing within an epoch**, because a deleted task is still an
 *   answer to "what was running at 3.2 seconds". Entities are closed, not
 *   removed.
 *
 * An epoch change clears the store: the same address in a new epoch is a new
 * entity, and pretending otherwise would join two runs of two different
 * firmwares.
 */
class RtosIdentityStore
{
   public:
	/* What the store could make of one event's identity. */
	struct Outcome
	{
		RtosEntityKey key;

		/* The target created a lifetime for a handle that already had one open.
		   Impossible in a correct target; reported rather than swallowed,
		   because the alternative is a silently merged history. */
		bool implicitClose = false;

		/* The entity was made up from an ordinary event: no creation was seen,
		   so the generation is unknown and the origin is the middle of the
		   session. */
		bool openedWithoutCreation = false;

		/* A lifetime that began or ended across a gap in the stream. The
		   identity is still usable, the numbers around it are not. */
		bool afterGap = false;
	};

	/* One lifetime, as the store remembers it. */
	struct Entity
	{
		RtosEntityKey key;
		std::string name;

		/* The name came from the target's dictionary rather than from a
		   snapshot, which is what makes it valid for history and not only for
		   the present. */
		bool nameFromDictionary = false;

		uint64_t firstSeenTicks = 0;

		/* The generation is a guess: either the lifetime was never announced,
		   or the announcement was lost. */
		bool generationUnknown = false;

		/* The lifetime began or ended across a gap: the announcement that would
		   have told us its generation is the one that did not arrive. */
		bool afterGap = false;

		bool closed = false;
	};

	/* Feeds one event's identity part in. `type` decides whether a lifetime is
	   opened, closed, or merely referenced. */
	Outcome observe(const RtosEntityRef& ref, RtosTraceEventType type, uint64_t epoch);

	/* A gap in the sequence. Any lifetime that opens or closes after this point
	   is marked, because the creation or deletion that belongs in the gap was
	   never seen. */
	void noteGap();

	/* The entity a key belongs to, or null. */
	const Entity* find(const RtosEntityKey& key) const;

	/*
	 * Every lifetime the store knows, in key order.
	 *
	 * Read only, for the one caller that has to reconcile a round of the table
	 * against the events: it knows addresses, not generations, and an address
	 * on its own only identifies an entity while one lifetime of it is open.
	 */
	const std::map<RtosEntityKey, Entity>& all() const
	{
		return entities;
	}

	/* The open lifetime of a handle, or null. Used by the reconciler, which is
	   the only thing that asks "who is 0x20001B60 right now". */
	const Entity* openEntityFor(RtosEntityKind kind, uint32_t rawHandle) const;

	/* Fills in a display name for an entity created from an ordinary event.
	   Only the name: the identity was decided when the entity was opened and a
	   later snapshot cannot improve it. */
	void nameFromSnapshot(const RtosEntityKey& key, const std::string& name);

	/* A name that came from the target's dictionary, valid for the whole
	   lifetime it belongs to. */
	void nameFromDictionary(const RtosEntityKey& key, const std::string& name);

	/*
	 * Puts one lifetime back exactly as it was written down.
	 *
	 * Only a recording being read back does this. In a live session the store
	 * decides generations as it watches the stream; a file already holds those
	 * decisions, and making them again from the events in it would number the
	 * lifetimes differently - a handle whose creation was lost would come out
	 * as generation zero instead of the generation the live view showed.
	 */
	void restore(const Entity& entity);

	void clear();

	size_t entityCount() const
	{
		return entities.size();
	}

	size_t openCount() const;

   private:
	struct HandleState
	{
		uint32_t openGeneration = 0;
		uint32_t highestGeneration = 0;
		bool open = false;
	};

	RtosEntityKey open(const RtosEntityRef& ref, uint64_t epoch, uint32_t generation, bool generationUnknown, bool afterGap);

	std::map<RtosEntityKey, Entity> entities;
	std::map<RtosEntityKey, HandleState> handles;  /* keyed on (epoch, kind, handle) with generation 0 */

	bool gapPending = false;
};

#endif
