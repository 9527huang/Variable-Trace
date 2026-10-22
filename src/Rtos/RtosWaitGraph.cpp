#include "RtosWaitGraph.hpp"

#include <algorithm>

namespace
{
	const RtosTask* findTask(const RtosSnapshot& snapshot, uint32_t address)
	{
		const auto found = std::find_if(snapshot.tasks.begin(), snapshot.tasks.end(),
		                                [address](const RtosTask& task) { return task.tcbAddress == address; });

		return found == snapshot.tasks.end() ? nullptr : &*found;
	}

	/* The object a task is waiting on, by name. A kernel names objects, not
	   handles, in its wait lists, and the registry is the only place those names
	   are readable - so a wait on an unregistered object cannot be followed and
	   the chain stops there rather than guessing. */
	const RtosSyncObject* findObject(const RtosSnapshot& snapshot, const std::string& name)
	{
		if (name.empty())
			return nullptr;

		const auto found = std::find_if(snapshot.syncObjects.begin(), snapshot.syncObjects.end(),
		                                [&name](const RtosSyncObject& object) { return object.name == name; });

		return found == snapshot.syncObjects.end() ? nullptr : &*found;
	}

	RtosWaitGraph::Node taskNode(const RtosTask& task)
	{
		RtosWaitGraph::Node node;

		node.kind = RtosWaitGraph::NodeKind::Task;
		node.address = task.tcbAddress;
		node.name = task.name;
		node.state = task.state;
		node.hasState = true;
		node.priority = task.rawPriority;
		node.hasPriority = task.hasPriority;

		return node;
	}

	RtosWaitGraph::Node objectNode(const RtosSyncObject& object)
	{
		RtosWaitGraph::Node node;

		node.kind = RtosWaitGraph::NodeKind::Object;
		node.address = object.address;
		node.name = object.name;
		node.syncKind = object.kind;
		node.capacity = object.capacity;
		node.items = object.items;
		node.hasCounts = object.hasCounts;
		node.waitingOnThis = object.waitingToReceive;  /* the wait list, not the send list */
		node.hasWaiters = object.hasWaiters;

		return node;
	}

	size_t indexOfTask(const std::vector<RtosWaitGraph::Node>& nodes, uint32_t address)
	{
		for (size_t index = 0; index < nodes.size(); index++)
		{
			if (nodes[index].kind == RtosWaitGraph::NodeKind::Task && nodes[index].address == address)
				return index;
		}

		return nodes.size();
	}
}  // namespace

RtosWaitGraph::Chain RtosWaitGraph::from(const RtosSnapshot& snapshot, uint32_t task, size_t limit)
{
	Chain chain;

	const RtosTask* first = findTask(snapshot, task);

	if (first == nullptr)
		return chain;

	chain.nodes.push_back(taskNode(*first));

	/* The task asked about is the one this chain is for, so a ring that comes
	   back to it is a ring even when the walk started in the middle of one. */
	const RtosTask* current = first;

	while (chain.nodes.size() < limit)
	{
		const RtosSyncObject* object = findObject(snapshot, current->waitingOn);

		/* Waiting for a name the registry does not hold: nothing to follow, and
		   nothing to say about it either - the box for the object cannot be
		   filled in, so it is not drawn. */
		if (object == nullptr)
			break;

		Step wait;
		wait.from = chain.nodes.size() - 1;
		wait.to = chain.nodes.size();
		wait.heldBy = false;

		chain.nodes.push_back(objectNode(*object));
		chain.steps.push_back(wait);

		if (object->holder == 0 || !object->hasHolder)
			break;

		const size_t back = indexOfTask(chain.nodes, object->holder);

		if (back < chain.nodes.size())
		{
			/* The ring: the holder is already in the picture. The step points
			   at the box that is there instead of a second copy of it. */
			Step close;
			close.from = chain.nodes.size() - 1;
			close.to = back;
			close.heldBy = true;
			close.closesCycle = true;

			chain.steps.push_back(close);
			chain.cycle = true;

			break;
		}

		const RtosTask* holder = findTask(snapshot, object->holder);

		if (holder == nullptr || chain.nodes.size() + 1 > limit)
			break;

		Step holds;
		holds.from = chain.nodes.size() - 1;
		holds.to = chain.nodes.size();
		holds.heldBy = true;

		chain.nodes.push_back(taskNode(*holder));
		chain.steps.push_back(holds);

		current = holder;
	}

	return chain;
}
