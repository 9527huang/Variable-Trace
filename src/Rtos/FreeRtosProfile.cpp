#include "FreeRtosProfile.hpp"

RtosProfileBase::Type FreeRtosProfile::getType() const
{
	return Type::FreeRtos;
}

std::string FreeRtosProfile::getDescription() const
{
	return "FreeRTOS";
}

const char* FreeRtosProfile::testedKernelVersion()
{
	/* The version the offsets in the fallback table were measured against.
	   Reported beside the detection result; nothing branches on it. */
	return "V11.1.0";
}

bool FreeRtosProfile::higherNumberIsHigherPriority() const
{
	return true;
}

uint32_t FreeRtosProfile::taskNameCapacity() const
{
	return 16;
}

const std::vector<RtosProfileBase::SymbolSpec>& FreeRtosProfile::getSymbols() const
{
	/* The right hand column is what the interface says when the symbol cannot
	   be found. A missing required symbol stops the snapshot and names itself;
	   a missing optional one removes a feature and nothing else. */
	static const std::vector<SymbolSpec> table = {
		{"currentTask", "pxCurrentTCB", Need::Required, "the task the scheduler is running"},
		{"readyLists", "pxReadyTasksLists", Need::Required, "the per priority ready lists"},
		{"topReadyPriority", "uxTopReadyPriority", Need::Optional, "highest ready priority, absent when optimised task selection is on"},
		{"delayedList", "pxDelayedTaskList", Need::Required, "the list a task waiting on time is on"},
		{"overflowDelayedList", "pxOverflowDelayedTaskList", Need::Required, "the other half of the delay lists"},
		{"delayedListStorage1", "xDelayedTaskList1", Need::Optional, "delay list storage, for comparing a container pointer directly"},
		{"delayedListStorage2", "xDelayedTaskList2", Need::Optional, "the second delay list storage"},
		{"pendingReadyList", "xPendingReadyList", Need::Optional, "tasks made ready while the scheduler was suspended"},
		{"suspendedList", "xSuspendedTaskList", Need::Required, "suspended tasks, and tasks blocked forever on an object never given"},
		{"waitingTermination", "xTasksWaitingTermination", Need::Optional, "deleted tasks whose memory is not reclaimed yet"},
		{"tickCount", "xTickCount", Need::Required, "kernel time base, needed for remaining wait and for the timeline"},
		{"currentNumberOfTasks", "uxCurrentNumberOfTasks", Need::Optional, "for checking a snapshot against what the kernel thinks"},
		{"schedulerRunning", "xSchedulerRunning", Need::Optional, "explains ready tasks with nothing running"},
		{"queueRegistry", "xQueueRegistry", Need::Optional, "the only list of queues the kernel keeps; absent when the registry is switched off"},
		{"schedulerSuspended", "uxSchedulerSuspended", Need::Optional, "explains ready tasks with nothing running"},
	};

	return table;
}

const std::vector<RtosProfileBase::FieldSpec>& FreeRtosProfile::getFields() const
{
	static const std::vector<FieldSpec> table = {
		/* Queue_t, which is also what a semaphore and a mutex are */
		{"queue.head", "Queue_t", {"pcHead"}, Need::Required, "the kind marker: null for a mutex, the object itself for a semaphore, the storage for a queue"},
		{"queue.length", "Queue_t", {"uxLength"}, Need::Required, "how many items it can hold, which is the semaphore's limit"},
		{"queue.items", "Queue_t", {"uxMessagesWaiting"}, Need::Required, "how much of that is in use, which is the queue's depth"},
		{"queue.waitingToSend", "Queue_t", {"xTasksWaitingToSend"}, Need::Required, "tasks blocked because there was no room"},
		{"queue.waitingToReceive", "Queue_t", {"xTasksWaitingToReceive"}, Need::Required, "tasks blocked because there was nothing to take"},

		/*
		 * These two sit inside the union a queue starts with, and `ptype /o`
		 * does not open a nested structure: the dump of Queue_t shows the
		 * union and the names of its members, but not their internals. So the
		 * offsets for these come from the profile's own table and are marked
		 * as fallbacks, and the name says which structure they belong to so
		 * that a reader can find them by hand if one ever has to.
		 */
		{"semaphore.holder", "Queue_t", {"xMutexHolder"}, Need::Optional, "the task holding a mutex, inside the union at the head"},
		{"semaphore.recursiveCount", "Queue_t", {"uxRecursiveCallCount"}, Need::Optional, "how many times that task has taken it"},

		/* QueueRegistryItem_t, the only list of queues the kernel keeps */
		{"registry.name", "QueueRegistryItem_t", {"pcQueueName"}, Need::Optional, "the name an object was registered under"},
		{"registry.handle", "QueueRegistryItem_t", {"xHandle"}, Need::Optional, "the object itself"},

		/* List_t */
		{"list.numberOfItems", "List_t", {"uxNumberOfItems"}, Need::Required, "item count, for checking a walk against the kernel"},
		{"list.index", "List_t", {"pxIndex"}, Need::Optional, "where the last walk stopped"},
		{"list.end", "List_t", {"xListEnd"}, Need::Required, "the sentinel a walk starts from"},

		/* ListItem_t */
		{"listItem.value", "ListItem_t", {"xItemValue"}, Need::Required, "wake time on a delay list, priority on a ready list"},
		{"listItem.next", "ListItem_t", {"pxNext"}, Need::Required, "next item in the walk"},
		{"listItem.previous", "ListItem_t", {"pxPrevious"}, Need::Optional, "previous item in the walk"},
		{"listItem.owner", "ListItem_t", {"pvOwner"}, Need::Required, "the task control block this item belongs to"},
		{"listItem.container", "ListItem_t", {"pxContainer", "pvContainer"}, Need::Required, "the list this item is on right now; renamed in V11"},

		/* MiniListItem_t, the sentinel inside List_t */
		{"miniItem.value", "MiniListItem_t", {"xItemValue"}, Need::Optional, "sentinel marker on a ready list"},
		{"miniItem.next", "MiniListItem_t", {"pxNext"}, Need::Required, "first real item, and the answer for an empty list"},
		{"miniItem.previous", "MiniListItem_t", {"pxPrevious"}, Need::Optional, "last real item"},

		/* TCB_t */
		{"tcb.topOfStack", "TCB_t", {"pxTopOfStack"}, Need::Optional, "where the task resumes"},
		{"tcb.stateListItem", "TCB_t", {"xStateListItem"}, Need::Required, "which list the task is on, and therefore its state"},
		{"tcb.eventListItem", "TCB_t", {"xEventListItem"}, Need::Required, "which object the task is waiting on"},
		{"tcb.priority", "TCB_t", {"uxPriority"}, Need::Required, "current priority; differs from the base one while inheritance is active"},
		{"tcb.basePriority", "TCB_t", {"uxBasePriority"}, Need::Optional, "needs configUSE_MUTEXES"},
		{"tcb.stack", "TCB_t", {"pxStack"}, Need::Optional, "lowest address of the task stack"},
		{"tcb.endOfStack", "TCB_t", {"pxEndOfStack"}, Need::Optional, "needs configRECORD_STACK_HIGH_ADDRESS"},
		{"tcb.taskName", "TCB_t", {"pcTaskName"}, Need::Required, "the name, fixed length and zero terminated"},
		{"tcb.tcbNumber", "TCB_t", {"uxTCBNumber"}, Need::Optional, "needs configUSE_TRACE_FACILITY; usable as a generation"},
		{"tcb.taskNumber", "TCB_t", {"uxTaskNumber"}, Need::Optional, "needs configUSE_TRACE_FACILITY"},
		{"tcb.mutexesHeld", "TCB_t", {"uxMutexesHeld"}, Need::Optional, "needs configUSE_MUTEXES"},
	};

	return table;
}

RtosLayoutTable FreeRtosProfile::getFallbackLayout() const
{
	using Origin = RtosLayoutTable::Origin;

	/* Every number here was measured on one build and is only correct for a
	   build that left the list integrity checking options off. Marking the
	   origin as `Fallback` is what keeps the interface from presenting them as
	   something they are not. */
	constexpr Origin fallback = Origin::Fallback;

	RtosLayoutTable layout;

	layout.addStruct("List_t", 20, fallback);
	layout.addStruct("ListItem_t", 20, fallback);
	layout.addStruct("MiniListItem_t", 12, fallback);
	layout.addStruct("TCB_t", 96, fallback);

	/* Read off one build of the kernel with this project's configuration. A
	   different build moves them, which is exactly why every other offset is
	   asked of the debug information first. */
	layout.addStruct("Queue_t", 72, fallback);
	layout.addStruct("QueueRegistryItem_t", 8, fallback);

	/* List_t: the index pointer sits between the count and the sentinel. */
	layout.addField("List_t", "uxNumberOfItems", 0, fallback);

	layout.addField("Queue_t", "pcHead", 0, fallback);
	layout.addField("Queue_t", "xTasksWaitingToSend", 16, fallback);
	layout.addField("Queue_t", "xTasksWaitingToReceive", 36, fallback);
	layout.addField("Queue_t", "uxMessagesWaiting", 56, fallback);
	layout.addField("Queue_t", "uxLength", 60, fallback);

	/* The union at the head of a queue, which `ptype /o` does not open. */
	layout.addField("Queue_t", "xMutexHolder", 8, fallback);
	layout.addField("Queue_t", "uxRecursiveCallCount", 12, fallback);

	layout.addField("QueueRegistryItem_t", "pcQueueName", 0, fallback);
	layout.addField("QueueRegistryItem_t", "xHandle", 4, fallback);
	layout.addField("List_t", "pxIndex", 4, fallback);
	layout.addField("List_t", "xListEnd", 8, fallback);

	layout.addField("ListItem_t", "xItemValue", 0, fallback);
	layout.addField("ListItem_t", "pxNext", 4, fallback);
	layout.addField("ListItem_t", "pxPrevious", 8, fallback);
	layout.addField("ListItem_t", "pvOwner", 12, fallback);

	/* The container pointer kept its offset and changed its name in V11. Both
	   spellings are registered so that the field spec finds the same byte
	   whichever release the symbol file came from. */
	layout.addField("ListItem_t", "pxContainer", 16, fallback);
	layout.addField("ListItem_t", "pvContainer", 16, fallback);

	layout.addField("MiniListItem_t", "xItemValue", 0, fallback);
	layout.addField("MiniListItem_t", "pxNext", 4, fallback);
	layout.addField("MiniListItem_t", "pxPrevious", 8, fallback);

	layout.addField("TCB_t", "pxTopOfStack", 0, fallback);
	layout.addField("TCB_t", "xStateListItem", 4, fallback);
	layout.addField("TCB_t", "xEventListItem", 24, fallback);
	layout.addField("TCB_t", "uxPriority", 44, fallback);
	layout.addField("TCB_t", "pxStack", 48, fallback);
	layout.addField("TCB_t", "pcTaskName", 52, fallback);
	layout.addField("TCB_t", "pxEndOfStack", 68, fallback);
	layout.addField("TCB_t", "uxTCBNumber", 72, fallback);
	layout.addField("TCB_t", "uxTaskNumber", 76, fallback);
	layout.addField("TCB_t", "uxBasePriority", 80, fallback);
	layout.addField("TCB_t", "uxMutexesHeld", 84, fallback);

	return layout;
}
