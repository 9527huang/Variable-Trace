#include "RtosDiagnostics.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace
{
	std::string describe(const RtosTask& task)
	{
		if (!task.name.empty())
			return task.name;

		char buffer[16] = {};
		std::snprintf(buffer, sizeof(buffer), "0x%08X", task.tcbAddress);

		return buffer;
	}

	std::string describe(const RtosSyncObject& object)
	{
		if (!object.name.empty())
			return object.name;

		char buffer[16] = {};
		std::snprintf(buffer, sizeof(buffer), "0x%08X", object.address);

		return buffer;
	}

	const RtosTask* findTaskByAddress(const RtosSnapshot& snapshot, uint32_t address)
	{
		for (const RtosTask& task : snapshot.tasks)
		{
			if (task.tcbAddress == address)
				return &task;
		}

		return nullptr;
	}
}  // namespace

std::vector<RtosDiagnostics::Finding> RtosDiagnostics::diagnose(const RtosSnapshot& snapshot)
{
	std::vector<Finding> findings;

	/* Names to objects: a task says what it waits on by name, and the holder is
	   only known per object. */
	std::map<std::string, const RtosSyncObject*> objects;

	for (const RtosSyncObject& object : snapshot.syncObjects)
	{
		if (!object.name.empty())
			objects[object.name] = &object;
	}

	/*
	 * The lock a task is waiting for, the holder of it, and whether that is
	 * known at all. A wait on an object the round did not see, or on one whose
	 * holder it could not read, ends the walk rather than guessing.
	 */
	const auto heldBy = [&](const RtosTask& task) -> const RtosTask*
	{
		if (task.waitingOn.empty())
			return nullptr;

		const auto object = objects.find(task.waitingOn);

		if (object == objects.end() || !object->second->hasHolder || object->second->holder == 0)
			return nullptr;

		return findTaskByAddress(snapshot, object->second->holder);
	};

	/*
	 * Inheritance, read the way the waiting goes: the waiter, the object, and
	 * the task holding it.
	 *
	 * The task whose priority has been raised is the one holding the lock - the
	 * kernel lends it the priority of whoever is waiting - so the raised value
	 * is looked for on holders, and the waiter is found from the object.
	 */
	for (const RtosSyncObject& object : snapshot.syncObjects)
	{
		if (object.name.empty() || !object.hasHolder || object.holder == 0)
			continue;

		const RtosTask* holder = findTaskByAddress(snapshot, object.holder);

		if (holder == nullptr || !holder->hasBasePriority || !holder->hasPriority || holder->rawPriority == holder->basePriority)
			continue;

		for (const RtosTask& waiter : snapshot.tasks)
		{
			if (waiter.waitingOn != object.name)
				continue;

			Finding finding;
			finding.kind = Kind::PriorityInheritance;

			finding.chain = {describe(waiter), object.name, describe(*holder)};
			finding.tasks = {waiter.tcbAddress, 0, holder->tcbAddress};

			findings.push_back(finding);
		}
	}

	/*
	 * Deadlock: walk from each task along "waits for a lock, which its holder
	 * holds" and see whether the walk comes back on itself.
	 *
	 * Every task waits for at most one thing, so the graph has one edge out of
	 * each node and a walk either ends or runs into a cycle it can never leave.
	 * The cycle is the report; the tail that led into it is not part of it.
	 * Each ring is written once, from its smallest address - otherwise every
	 * task on the ring would report the same deadlock from a different place.
	 */
	std::set<uint32_t> reported;

	for (const RtosTask& start : snapshot.tasks)
	{
		std::vector<const RtosTask*> path;
		std::map<uint32_t, size_t> position;

		const RtosTask* current = &start;

		for (size_t step = 0; step <= snapshot.tasks.size(); step++)
		{
			const auto seen = position.find(current->tcbAddress);

			if (seen != position.end())
			{
				const size_t first = seen->second;

				/* A task waiting for something it holds itself needs a
				   recursive mutex and a mistake; it is not a ring and the
				   kernel would not let it block. */
				if (path.size() - first < 2)
					break;

				uint32_t smallest = path[first]->tcbAddress;

				for (size_t index = first; index < path.size(); index++)
					smallest = std::min(smallest, path[index]->tcbAddress);

				if (reported.count(smallest) == 0)
				{
					reported.insert(smallest);

					Finding finding;
					finding.kind = Kind::SuspectedDeadlock;

					for (size_t index = first; index < path.size(); index++)
					{
						finding.chain.push_back(describe(*path[index]));
						finding.tasks.push_back(path[index]->tcbAddress);

						if (!path[index]->waitingOn.empty())
						{
							finding.chain.push_back(path[index]->waitingOn);
							finding.tasks.push_back(0);
						}
					}

					/* The chain closes where it began; naming the first task
					   again is what makes it read as a ring rather than a
					   list. */
					finding.chain.push_back(describe(*path[first]));
					finding.tasks.push_back(path[first]->tcbAddress);

					findings.push_back(finding);
				}

				break;
			}

			position[current->tcbAddress] = path.size();
			path.push_back(current);

			const RtosTask* next = heldBy(*current);

			if (next == nullptr)
				break;

			current = next;
		}
	}

	return findings;
}
