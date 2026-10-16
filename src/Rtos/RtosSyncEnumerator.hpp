#ifndef _RTOSSYNCENUMERATOR_HPP
#define _RTOSSYNCENUMERATOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "RtosListWalker.hpp"
#include "RtosProfileBase.hpp"

/*
 * What a synchronisation object is, in one vocabulary.
 *
 * Queues, semaphores and mutexes are the same object in FreeRTOS and are told
 * apart by what the first pointer of that object holds, which is a fact about
 * one kernel's source rather than about the reader. It stops here, the same way
 * the task states do.
 *
 * A recursive mutex is not a fourth kind. Nothing in the object says whether it
 * can be taken twice by the same task: the kernel marks every mutex the same
 * way and only uses the recursion counter for the ones created recursive. So a
 * recursive mutex is reported as a mutex, and the counter is carried beside it
 * for a reader who wants to see it.
 */
enum class RtosSyncKind
{
	Queue,
	Semaphore,
	Mutex,
	Unknown,
};

const char* toString(RtosSyncKind kind);

struct RtosSyncObject
{
	/* The object's own address, which is what the registry holds a handle to. */
	uint32_t address = 0;

	/* The name it was registered under. An object that is not registered has
	   no name to show, and the kernel keeps no list of them to find it by. */
	std::string name;

	RtosSyncKind kind = RtosSyncKind::Unknown;

	/* Capacity and how much of it is in use. For a semaphore this is the count
	   and its limit; for a mutex the limit is one and the count says whether
	   it is held. */
	uint32_t capacity = 0;
	uint32_t items = 0;
	bool hasCounts = false;

	/* How many tasks are waiting on each side. These come from the item count
	   of the two wait lists rather than from walking them: the number is what
	   the panel shows, and walking would cost a read per waiting task for it. */
	uint32_t waitingToSend = 0;
	uint32_t waitingToReceive = 0;
	bool hasWaiters = false;

	/* The addresses of the two wait lists, so that a task's own list pointer
	   can be matched against them. Both are inside the object, at offsets the
	   kernel's layout gives. */
	uint32_t waitingToSendList = 0;
	uint32_t waitingToReceiveList = 0;

	/* For a mutex, the task that holds it and how many times it has taken it.
	   Zero means nobody holds it; `hasHolder` separates that from a mutex
	   whose holder could not be read. */
	uint32_t holder = 0;
	uint32_t recursiveCount = 0;
	bool hasHolder = false;
};

struct RtosSyncEnumeration
{
	std::vector<RtosSyncObject> objects;

	/* One sentence per object that could not be read, and per registry slot
	   that could not be read at all. */
	std::vector<std::string> problems;

	/* False when any slot was left unread, so that a caller does not present a
	   partial list as the whole picture. */
	bool complete = true;

	uint32_t slotsRead = 0;
	uint32_t objectsRead = 0;
};

class RtosSyncEnumerator
{
   public:
	/* Where the objects are and how many slots the registry has.
	 *
	 * The registry is a fixed size array whose length is a build time constant
	 * with no symbol of its own, so the count has to come from the caller, the
	 * same way the ready list count does. */
	struct RegistrySource
	{
		uint32_t address = 0;
		uint32_t capacity = 0;
	};

	/*
	 * Reads every registered object.
	 *
	 * A kernel keeps no list of its queues: the only way to find them is the
	 * registry, which holds the objects someone asked to have recorded. An
	 * object created but never registered is invisible here, and that is a
	 * property of the kernel rather than a gap in this reader.
	 */
	static RtosSyncEnumeration enumerate(IRtosMemory& memory,
	                                     const RtosProfileBase::Resolution& layout,
	                                     const RegistrySource& registry,
	                                     const RtosWalkLimits& limits = {});
};

#endif
