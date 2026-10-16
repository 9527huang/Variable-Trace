#include "RtosSampler.hpp"

#include <cstring>
#include <utility>

#include "RtosSyncEnumerator.hpp"

namespace
{
	/* A stand in for an offset that could not be resolved. Zero would be a real
	   offset, so it cannot be used to mean "absent". */
	constexpr uint32_t absentOffset = 0xFFFFFFFFu;

	/* Enough for a control block on any kernel this is likely to meet, and
	   small enough to live on the stack of the acquisition thread. */
	constexpr uint32_t controlBlockBufferSize = 512;

	uint32_t wordAt(const uint8_t* buffer, uint32_t offset)
	{
		uint32_t value = 0;
		std::memcpy(&value, buffer + offset, sizeof(value));

		return value;
	}
}  // namespace

RtosSampler::RtosSampler(std::shared_ptr<RtosProfileBase> profile) : profile(std::move(profile))
{
}

bool RtosSampler::prepare(const IElfParser::SymbolMap& symbols, const RtosLayoutTable& layout, const Settings& settings)
{
	prepared = false;
	error.clear();
	fixedListSources.clear();

	if (profile == nullptr)
	{
		error = "no kernel profile was selected";

		return false;
	}

	/* The layout came from the caller. When it was read out of debug
	   information the resolution records every field as measured, and when it
	   was not the profile's own numbers answer and are marked as fallbacks, so
	   a table built from this says which it is. */
	resolved = profile->resolve(symbols, layout);

	if (!resolved.complete())
	{
		error = resolved.summary();

		return false;
	}

	currentTaskSymbol = resolved.addressOf("currentTask");
	delayedListPointerSymbol = resolved.addressOf("delayedList");
	overflowDelayedListPointerSymbol = resolved.addressOf("overflowDelayedList");
	readyListsAddress = resolved.addressOf("readyLists");

	uint32_t listSize = 0;

	if (!layout.structSize("List_t", listSize) || listSize == 0)
	{
		error = "the profile does not state how large a kernel list is";

		return false;
	}

	readyListsStride = listSize;
	nameCapacity = profile->taskNameCapacity();

	if (settings.readControlBlocks)
	{
		if (!layout.structSize("TCB_t", controlBlockSize) || controlBlockSize == 0 || controlBlockSize > controlBlockBufferSize)
		{
			error = "the profile has no usable size for a task control block";

			return false;
		}

		if (!layout.fieldOffset("TCB_t", "pcTaskName", nameOffset) || !layout.fieldOffset("TCB_t", "uxPriority", priorityOffset) ||
		    !layout.fieldOffset("TCB_t", "pxStack", stackOffset) || !layout.fieldOffset("TCB_t", "pxEndOfStack", stackTopOffset))
		{
			error = "the profile's control block layout is missing a field the task table needs";

			return false;
		}

		/* These two need kernel options a project may not have turned on, so
		   their absence removes a column rather than the round. */
		basePriorityOffset = absentOffset;
		kernelNumberOffset = absentOffset;

		uint32_t offset = 0;

		if (layout.fieldOffset("TCB_t", "uxBasePriority", offset))
			basePriorityOffset = offset;

		if (layout.fieldOffset("TCB_t", "uxTCBNumber", offset))
			kernelNumberOffset = offset;

		/*
		 * Where the event item's container pointer lives.
		 *
		 * Taken apart into the two offsets it is made of, because the half that
		 * moves between builds is the first one: a list item is laid out the
		 * same way in every kernel, but where the item sits inside a control
		 * block is the build's business. Both halves are needed or neither is
		 * useful, so an incomplete layout leaves this absent and the round
		 * carries on without the column.
		 */
		eventListContainerOffset = absentOffset;

		uint32_t eventItemOffset = 0;
		uint32_t containerOffset = 0;

		if (resolved.offsetOf("tcb.eventListItem", eventItemOffset) && resolved.offsetOf("listItem.container", containerOffset))
			eventListContainerOffset = eventItemOffset + containerOffset;
	}

	/* The ready array, one list per priority. Its length is stated rather than
	   read: `configMAX_PRIORITIES` is a compile time constant and leaves no
	   trace in a symbol file, and walking past the end would read whatever
	   follows the array and report it as a priority that has tasks. */
	for (uint32_t priority = 0; priority < settings.readyListCapacity; priority++)
	{
		RtosTaskEnumerator::ListSource source;
		source.key = "ready." + std::to_string(priority);
		source.address = readyListsAddress + priority * readyListsStride;
		source.state = RtosTaskState::Ready;
		source.carriesPriority = true;

		fixedListSources.push_back(std::move(source));
	}

	/* The three lists whose addresses are their own. Each is optional: a
	   kernel built without the option has no such symbol, and the round simply
	   does not look at what does not exist. */
	const struct
	{
		const char* symbolKey;
		const char* listKey;
		RtosTaskState state;
	} fixedLists[] = {
		{"suspendedList", "suspended", RtosTaskState::Suspended},
		{"pendingReadyList", "pending ready", RtosTaskState::PendingReady},
		{"waitingTermination", "waiting termination", RtosTaskState::Deleted},
	};

	for (const auto& fixed : fixedLists)
	{
		if (!resolved.has(fixed.symbolKey))
			continue;

		RtosTaskEnumerator::ListSource source;
		source.key = fixed.listKey;
		source.address = resolved.addressOf(fixed.symbolKey);
		source.state = fixed.state;

		fixedListSources.push_back(std::move(source));
	}

	/* The registry is optional. A kernel built with its size set to zero has no
	   such array, and a round then carries no synchronisation objects rather
	   than reporting a failure. */
	if (resolved.has("queueRegistry"))
		queueRegistryAddress = resolved.addressOf("queueRegistry");

	prepared = true;

	return true;
}

bool RtosSampler::readWords(IRtosMemory& memory, const std::vector<uint32_t>& addresses, std::vector<uint32_t>& values) const
{
	values.clear();
	values.reserve(addresses.size());

	for (const uint32_t address : addresses)
	{
		/* An optional symbol that was not found leaves a zero here. A zero
		   address is not something a target keeps a kernel global at, so it
		   reads as "there is nothing to read" rather than as a failure. */
		if (address == 0)
		{
			values.push_back(0);

			continue;
		}

		uint8_t buffer[sizeof(uint32_t)] = {};

		if (!memory.read(address, buffer, sizeof(buffer)))
			return false;

		values.push_back(wordAt(buffer, 0));
	}

	return true;
}

bool RtosSampler::readTaskDetail(IRtosMemory& memory, uint32_t tcbAddress, uint32_t span, RtosTask& task) const
{
	if (tcbAddress == 0 || span == 0 || span > controlBlockBufferSize)
		return false;

	uint8_t buffer[controlBlockBufferSize] = {};

	if (!memory.read(tcbAddress, buffer, span))
		return false;

	if (nameOffset + nameCapacity <= span)
	{
		/* A fixed length field that may fill its whole width without a
		   terminator, so the copy stops at whichever comes first. */
		const char* raw = reinterpret_cast<const char*>(buffer + nameOffset);
		size_t length = 0;

		while (length < nameCapacity && raw[length] != '\0')
			length++;

		task.name.assign(raw, length);
	}

	if (priorityOffset + sizeof(uint32_t) <= span)
	{
		task.rawPriority = wordAt(buffer, priorityOffset);
		task.hasPriority = true;
		task.priorityRank = normalizedPriority(task.rawPriority, profile->higherNumberIsHigherPriority());
	}

	if (stackOffset + sizeof(uint32_t) <= span && stackTopOffset + sizeof(uint32_t) <= span)
	{
		task.stackBase = wordAt(buffer, stackOffset);
		task.stackTop = wordAt(buffer, stackTopOffset);
		task.hasStack = true;
	}

	if (basePriorityOffset != absentOffset && basePriorityOffset + sizeof(uint32_t) <= span)
	{
		task.basePriority = wordAt(buffer, basePriorityOffset);
		task.hasBasePriority = true;
	}

	/*
	 * The container of the event item, which is the only thing that says what a
	 * task waits for. A kernel keeps the two answers apart: the state item goes
	 * onto one of its own lists, the event item onto the object's wait list.
	 */
	if (eventListContainerOffset != absentOffset && eventListContainerOffset + sizeof(uint32_t) <= span)
	{
		task.eventList = wordAt(buffer, eventListContainerOffset);
		task.hasEventList = true;
	}


	if (kernelNumberOffset != absentOffset && kernelNumberOffset + sizeof(uint32_t) <= span)
	{
		task.kernelTaskNumber = wordAt(buffer, kernelNumberOffset);
		task.hasKernelTaskNumber = true;
	}

	return true;
}

RtosSampler::Round RtosSampler::collect(IRtosMemory& memory, const std::string& sessionId, const Settings& settings) const
{
	Round round;

	if (!prepared)
	{
		round.problems.push_back("the sampler has not been prepared against a symbol file");

		return round;
	}

	/* Three globals before anything can be walked: the task that is running,
	   and the two halves of the delay list. The delay pointers are re-read
	   every round because the kernel swaps them at a tick boundary; reusing
	   one round's answer would follow a list the kernel had already left. */
	std::vector<uint32_t> values;

	if (!readWords(memory, {currentTaskSymbol, delayedListPointerSymbol, overflowDelayedListPointerSymbol}, values) || values.size() != 3)
	{
		round.problems.push_back("a kernel global could not be read");

		return round;
	}

	const uint32_t currentTcb = values[0];
	const uint32_t delayedList = values[1];
	const uint32_t overflowDelayedList = values[2];

	std::vector<RtosTaskEnumerator::ListSource> sources = fixedListSources;

	if (delayedList != 0)
		sources.push_back({"delayed", delayedList, RtosTaskState::Blocked, false, true});

	if (overflowDelayedList != 0 && overflowDelayedList != delayedList)
		sources.push_back({"overflow delayed", overflowDelayedList, RtosTaskState::Blocked, false, true});

	RtosTaskEnumeration enumeration = RtosTaskEnumerator::enumerate(memory, resolved, sources, currentTcb, profile->higherNumberIsHigherPriority(), settings.walkLimits);

	/*
	 * The objects first, then their wait lists.
	 *
	 * The kernel's own lists do not hold every task: one blocked on a queue or
	 * a semaphore sits on that object's wait list instead, and walking the
	 * kernel's lists alone leaves it out of the table altogether. So the
	 * objects are read first, and their wait lists are then walked as extra
	 * sources with the state that being on one means.
	 *
	 * It costs a walk per wait list, and each of those holds the tasks that
	 * are blocked on that one object - usually none, occasionally one.
	 */
	RtosSyncEnumeration objects;

	if (queueRegistryAddress != 0 && settings.queueRegistryCapacity > 0)
	{
		objects = RtosSyncEnumerator::enumerate(memory, resolved, {queueRegistryAddress, settings.queueRegistryCapacity}, settings.walkLimits);

		if (!objects.objects.empty())
		{
			std::vector<RtosTaskEnumerator::ListSource> waitSources;

			waitSources.reserve(objects.objects.size() * 2);

			for (const RtosSyncObject& object : objects.objects)
			{
				const std::string name = object.name.empty() ? ("the object at 0x" + std::to_string(object.address)) : object.name;

				waitSources.push_back({name + " (waiting to send)", object.waitingToSendList, RtosTaskState::Blocked, false, false});
				waitSources.push_back({name + " (waiting to receive)", object.waitingToReceiveList, RtosTaskState::Blocked, false, false});
			}

			/* No current task pointer this time: a task blocked on an object
			   is not running, so nothing here can be promoted to running. */
			RtosTaskEnumerator::absorb(enumeration, RtosTaskEnumerator::enumerate(memory, resolved, waitSources, 0u, profile->higherNumberIsHigherPriority(), settings.walkLimits));
		}
	}

	round.problems = enumeration.problems;
	round.inconsistencies = enumeration.inconsistencies;

	if (!enumeration.complete)
	{
		/* A round that could not be read is not published: the previous one
		   stays, and the caller records that a round went missing. Half a
		   snapshot shown as a whole one is what the level exists to prevent.
		   A round that merely caught the target moving is not that - it is
		   published below, and its inconsistencies travel with it. */
		return round;
	}

	auto snapshot = std::make_shared<RtosSnapshot>();
	snapshot->tasks = enumeration.tasks;
	snapshot->problems = enumeration.problems;
	snapshot->inconsistencies = enumeration.inconsistencies;
	snapshot->sessionId = sessionId;
	snapshot->listsWalked = enumeration.listsWalked;
	snapshot->listsFailed = enumeration.listsFailed;

	/* Reading a running target is all this can claim. The other level needs
	   the target stopped, which is a switch of its own and not built yet. */
	snapshot->consistency = RtosConsistency::BestEffort;

	if (settings.readControlBlocks)
	{
		for (RtosTask& task : snapshot->tasks)
		{
			/* One control block that could not be read costs that task its
			   names and numbers, not the round. Whoever ran under a failing
			   read is still a task with a state and an address. */
			if (!readTaskDetail(memory, task.tcbAddress, controlBlockSize, task))
			{
				round.problems.push_back("the control block of the task at 0x" + std::to_string(task.tcbAddress) + " could not be read");

				continue;
			}

			/*
			 * A task that blocks with no timeout in sight keeps its state item on
			 * the suspended list rather than on a delay list: the kernel does that
			 * on purpose, so that resuming the scheduler cannot release it. Its
			 * event item is then the only thing that separates it from a task that
			 * really was suspended, and reading the two together is what the
			 * kernel's own task state query does.
			 */
			if (task.hasEventList && task.eventList != 0 && task.state == RtosTaskState::Suspended)
				task.state = RtosTaskState::Blocked;
		}
	}

	/*
	 * The synchronisation objects, when the kernel keeps a registry to find
	 * them by. A kernel built with no registry has no way to enumerate its
	 * queues at all, so its absence is not a problem to report: the round
	 * simply carries no objects, and the interface says so.
	 *
	 * They were already read, above, because the wait lists they own had to be
	 * walked before this point.
	 */
	if (queueRegistryAddress != 0 && settings.queueRegistryCapacity > 0)
	{
		snapshot->syncObjects = objects.objects;
		snapshot->queueRegistryAvailable = true;

		for (const std::string& problem : objects.problems)
			round.problems.push_back(problem);

		if (!objects.complete)
			round.problems.push_back("the queue registry was only partly read");
	}

	/* Written once, after every part of the round has had its say. A later
	   part that finds something to report would otherwise have its sentence
	   dropped, which is how a problem becomes invisible. */
	snapshot->problems = round.problems;

	/*
	 * What each waiting task is waiting for.
	 *
	 * It is worked out here, after both halves of the round are in hand,
	 * because it needs them together: a task says which list its event item is
	 * on, and an object says where its two wait lists are.
	 *
	 * The event item, not the state item. A state item is always on one of the
	 * kernel's own lists, so its container can never name an object - comparing
	 * it against the objects' wait lists is a join that cannot match, and an
	 * un-matchable join looks exactly like a target where nobody is waiting.
	 * The state item's container stays as the fallback for a cheap round, which
	 * reads no control block and so knows only which list it walked.
	 *
	 * Nothing extra is read to answer this. The wait lists are addresses inside
	 * blocks that were read for their counts, and the event item's container
	 * came back with the control block. It is a join on data already in hand,
	 * which is what makes a column like this affordable at all.
	 */
	for (RtosTask& task : snapshot->tasks)
	{
		const uint32_t waitedOnList = task.hasEventList ? task.eventList : task.waitingList;

		if (waitedOnList == 0)
			continue;

		for (const RtosSyncObject& object : snapshot->syncObjects)
		{
			if (waitedOnList != object.waitingToSendList && waitedOnList != object.waitingToReceiveList)
				continue;

			task.waitingOn = object.name;

			break;
		}
	}


	round.snapshot = std::move(snapshot);

	return round;
}
