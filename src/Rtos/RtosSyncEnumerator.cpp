#include "RtosSyncEnumerator.hpp"

#include <cstdio>
#include <utility>

namespace
{
	/* A registry slot is a name pointer and a handle, nothing else. */
	constexpr uint32_t registryEntrySize = 8;

	/* Enough of a queue object to hold everything this reader asks for.
	   The largest offset it touches is `uxLength` near the end, so the buffer
	   has to cover the whole object rather than the part that looked
	   interesting: a buffer that stops short reads past its own end, and what
	   comes back is whatever was on the stack. */
	constexpr uint32_t queueBlockBytes = 72;

	constexpr uint32_t maxNameBytes = 32;

	uint32_t readWord(const uint8_t* at)
	{
		return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) | (static_cast<uint32_t>(at[2]) << 16) |
		       (static_cast<uint32_t>(at[3]) << 24);
	}

	std::string hexOf(uint32_t value)
	{
		char buffer[16] = {};

		std::snprintf(buffer, sizeof(buffer), "0x%08X", value);

		return buffer;
	}

	/* The offsets this reader needs, gathered once so that the walk below is
	   not carrying ten of them around. */
	struct Offsets
	{
		uint32_t registryName = 0;
		uint32_t registryHandle = 0;

		uint32_t queueHead = 0;
		uint32_t queueLength = 0;
		uint32_t queueItems = 0;
		uint32_t queueWaitingToSend = 0;
		uint32_t queueWaitingToReceive = 0;

		uint32_t mutexHolder = 0;
		uint32_t recursiveCount = 0;

		uint32_t listItemCount = 0;
	};

	bool offsetOf(const RtosProfileBase::Resolution& layout, const char* key, uint32_t& offset)
	{
		const auto it = layout.fields.find(key);

		if (it == layout.fields.end() || !it->second.resolved)
			return false;

		offset = it->second.offset;

		return true;
	}

	bool readOffsets(const RtosProfileBase::Resolution& layout, Offsets& offsets)
	{
		return offsetOf(layout, "registry.name", offsets.registryName) && offsetOf(layout, "registry.handle", offsets.registryHandle) &&
		       offsetOf(layout, "queue.head", offsets.queueHead) && offsetOf(layout, "queue.length", offsets.queueLength) &&
		       offsetOf(layout, "queue.items", offsets.queueItems) && offsetOf(layout, "queue.waitingToSend", offsets.queueWaitingToSend) &&
		       offsetOf(layout, "queue.waitingToReceive", offsets.queueWaitingToReceive) && offsetOf(layout, "semaphore.holder", offsets.mutexHolder) &&
		       offsetOf(layout, "semaphore.recursiveCount", offsets.recursiveCount) && offsetOf(layout, "list.numberOfItems", offsets.listItemCount);
	}

	/* Reads the name the registry points at, up to the terminator. */
	std::string readName(IRtosMemory& memory, uint32_t pointer)
	{
		uint8_t bytes[maxNameBytes] = {};

		if (!memory.read(pointer, bytes, sizeof(bytes)))
			return {};

		std::string name;

		for (uint8_t byte : bytes)
		{
			if (byte == 0)
				break;

			/* A name is meant to be readable. Anything outside printable
			   ASCII means the pointer was not a string, and a row of garbage
			   in the table is worse than an empty name. */
			if (byte < 32 || byte > 126)
				return {};

			name.push_back(static_cast<char>(byte));
		}

		return name;
	}
}  // namespace

const char* toString(RtosSyncKind kind)
{
	switch (kind)
	{
	case RtosSyncKind::Queue:
		return "queue";

	case RtosSyncKind::Semaphore:
		return "semaphore";

	case RtosSyncKind::Mutex:
		return "mutex";

	default:
		return "unknown";
	}
}

RtosSyncEnumeration RtosSyncEnumerator::enumerate(IRtosMemory& memory,
                                                  const RtosProfileBase::Resolution& layout,
                                                  const RegistrySource& registry,
                                                  const RtosWalkLimits& limits)
{
	(void)limits;

	RtosSyncEnumeration result;

	if (registry.address == 0 || registry.capacity == 0)
	{
		result.complete = false;
		result.problems.push_back("the kernel keeps no queue registry, so no synchronisation object can be found");

		return result;
	}

	Offsets offsets;

	if (!readOffsets(layout, offsets))
	{
		result.complete = false;
		result.problems.push_back("the layout does not describe the queue structures");

		return result;
	}

	/* The registry is a name pointer and a handle per slot. A slot with no
	   handle was never used, which is not a problem to report. */
	for (uint32_t slot = 0; slot < registry.capacity; ++slot)
	{
		uint8_t entry[registryEntrySize] = {};

		if (!memory.read(registry.address + slot * registryEntrySize, entry, sizeof(entry)))
		{
			result.complete = false;
			result.problems.push_back("registry slot " + std::to_string(slot) + " could not be read");

			continue;
		}

		const uint32_t namePointer = readWord(entry + offsets.registryName);
		const uint32_t handle = readWord(entry + offsets.registryHandle);

		if (handle == 0)
			continue;

		++result.slotsRead;

		RtosSyncObject object;
		object.address = handle;
		object.name = readName(memory, namePointer);

		/* One read covers the head pointer, the counts, both wait lists and
		   the mutex holder: they all sit in the first bytes of the object. */
		uint8_t block[queueBlockBytes] = {};

		if (!memory.read(handle, block, sizeof(block)))
		{
			result.complete = false;
			result.problems.push_back("the object at " + hexOf(handle) + " could not be read");

			continue;
		}

		/*
		 * The kind is what the first pointer holds, and the three cases are
		 * three different meanings of the same field: the kernel writes a null
		 * there for a mutex, the object's own address for a semaphore, and the
		 * storage area for a queue.
		 */
		const uint32_t head = readWord(block + offsets.queueHead);

		if (head == 0)
			object.kind = RtosSyncKind::Mutex;
		else if (head == handle)
			object.kind = RtosSyncKind::Semaphore;
		else
			object.kind = RtosSyncKind::Queue;

		object.capacity = readWord(block + offsets.queueLength);
		object.items = readWord(block + offsets.queueItems);
		object.hasCounts = true;

		/* The number of waiters is the item count at the head of each wait
		   list. Walking those lists would cost a read per waiting task for a
		   number the panel shows as a count. */
		object.waitingToSend = readWord(block + offsets.queueWaitingToSend + offsets.listItemCount);
		object.waitingToReceive = readWord(block + offsets.queueWaitingToReceive + offsets.listItemCount);
		object.hasWaiters = true;

		/* Kept so that a task's list pointer can be matched against them later.
		   Neither costs a read: both are addresses inside a block that has
		   already been read. */
		object.waitingToSendList = object.address + offsets.queueWaitingToSend;
		object.waitingToReceiveList = object.address + offsets.queueWaitingToReceive;

		if (object.kind == RtosSyncKind::Mutex)
		{
			object.holder = readWord(block + offsets.mutexHolder);
			object.recursiveCount = readWord(block + offsets.recursiveCount);
			object.hasHolder = true;
		}

		++result.objectsRead;
		result.objects.push_back(std::move(object));
	}

	return result;
}
