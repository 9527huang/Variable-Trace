#include "RtosListWalker.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace
{
	struct Offsets
	{
		uint32_t listCount = 0;
		uint32_t listEnd = 0;
		uint32_t miniNext = 0;
		uint32_t itemValue = 0;
		uint32_t itemNext = 0;
		uint32_t itemPrevious = 0;
		uint32_t itemOwner = 0;
		uint32_t itemContainer = 0;
	};

	bool readWord(IRtosMemory& memory, uint32_t address, uint32_t& value)
	{
		uint8_t buffer[sizeof(uint32_t)] = {};

		if (!memory.read(address, buffer, sizeof(buffer)))
			return false;

		std::memcpy(&value, buffer, sizeof(value));

		return true;
	}

	/* How much of an item to ask for: far enough to cover the last field the
	   walk reads. Asking for a fixed twenty would be wrong on a build whose
	   integrity checking options put padding in front of the items. */
	uint32_t itemSpanOf(const Offsets& offsets)
	{
		const uint32_t last = std::max({offsets.itemValue, offsets.itemNext, offsets.itemPrevious, offsets.itemOwner, offsets.itemContainer});

		return last + static_cast<uint32_t>(sizeof(uint32_t));
	}

	bool readItem(IRtosMemory& memory, uint32_t address, const Offsets& offsets, uint32_t span, RtosListWalker::Item& item)
	{
		uint8_t buffer[64] = {};

		if (span > sizeof(buffer))
			return false;

		if (!memory.read(address, buffer, span))
			return false;

		item.address = address;
		std::memcpy(&item.value, buffer + offsets.itemValue, sizeof(uint32_t));
		std::memcpy(&item.next, buffer + offsets.itemNext, sizeof(uint32_t));
		std::memcpy(&item.previous, buffer + offsets.itemPrevious, sizeof(uint32_t));
		std::memcpy(&item.owner, buffer + offsets.itemOwner, sizeof(uint32_t));
		std::memcpy(&item.container, buffer + offsets.itemContainer, sizeof(uint32_t));

		return true;
	}
}  // namespace

RtosListWalker::Walk RtosListWalker::walk(IRtosMemory& memory, const RtosProfileBase::Resolution& layout, uint32_t listAddress, const RtosWalkLimits& limits)
{
	Walk walk;
	walk.listAddress = listAddress;

	Offsets offsets;

	if (!layout.offsetOf("list.numberOfItems", offsets.listCount) || !layout.offsetOf("list.end", offsets.listEnd) || !layout.offsetOf("miniItem.next", offsets.miniNext) ||
	    !layout.offsetOf("listItem.value", offsets.itemValue) || !layout.offsetOf("listItem.next", offsets.itemNext) || !layout.offsetOf("listItem.previous", offsets.itemPrevious) ||
	    !layout.offsetOf("listItem.owner", offsets.itemOwner) || !layout.offsetOf("listItem.container", offsets.itemContainer))
	{
		walk.result = Result::LayoutIncomplete;

		return walk;
	}

	/* The sentinel sits inside the list head, so the walk starts from the
	   pointer it holds rather than from the head itself. */
	const uint32_t endAddress = listAddress + offsets.listEnd;

	if (!readWord(memory, listAddress + offsets.listCount, walk.declaredCount))
	{
		walk.result = Result::ReadingFailed;

		return walk;
	}

	uint32_t current = 0;

	if (!readWord(memory, endAddress + offsets.miniNext, current))
	{
		walk.result = Result::ReadingFailed;

		return walk;
	}

	const uint32_t span = itemSpanOf(offsets);

	std::unordered_set<uint32_t> seen;
	uint32_t previous = endAddress;

	while (current != endAddress)
	{
		if (walk.items.size() >= limits.maxItems)
		{
			walk.result = Result::TooManyItems;

			return walk;
		}

		/* A chain that folds back on itself would otherwise be walked until the
		   item limit, which for a two node loop means four thousand reads to
		   learn what one comparison would have said. */
		if (!seen.insert(current).second)
		{
			walk.result = Result::Loop;

			return walk;
		}

		Item item;

		if (!readItem(memory, current, offsets, span, item))
		{
			walk.result = Result::ReadingFailed;

			return walk;
		}

		/* Where the item says it came from has to be where the walk actually
		   came from. A target that re-linked the list between two reads shows
		   up here rather than as a plausible looking wrong answer. */
		if (item.previous != previous)
		{
			walk.result = Result::LinksDisagree;

			return walk;
		}

		walk.items.push_back(item);
		previous = current;
		current = item.next;
	}

	walk.countDisagrees = walk.items.size() != walk.declaredCount;

	return walk;
}

std::string RtosListWalker::describe(Result result)
{
	switch (result)
	{
		case Result::Ok:
			return "the list was walked to its end";

		case Result::ReadingFailed:
			return "a read failed while walking the list";

		case Result::LayoutIncomplete:
			return "the profile has no offset for a field the walk needs";

		case Result::Loop:
			return "the chain points back at a node it already visited";

		case Result::TooManyItems:
			return "the chain is longer than the item limit allows";

		case Result::LinksDisagree:
			return "a node's backward pointer does not lead back to the previous node";
	}

	return "the list walk stopped for a reason it cannot name";
}
