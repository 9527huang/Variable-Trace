#include "RtosTableModel.hpp"

#include <algorithm>
#include <cctype>

namespace
{
	bool containsIgnoringCase(const std::string& haystack, const std::string& needle)
	{
		if (needle.empty())
			return true;

		const auto equalIgnoringCase = [](char left, char right)
		{
			return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
		};

		return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), equalIgnoringCase) != haystack.end();
	}

	bool matchesState(RtosTaskState state, const std::vector<RtosTaskState>& states)
	{
		return states.empty() || std::find(states.begin(), states.end(), state) != states.end();
	}

	int compareNames(const std::string& left, const std::string& right)
	{
		/* A task whose name could not be read sorts last rather than first.
		   An empty name is missing data, not a name that comes before every
		   other name, and putting it at the top would have it noticed least. */
		if (left.empty() != right.empty())
			return left.empty() ? 1 : -1;

		return left.compare(right);
	}

	int signOf(int64_t left, int64_t right)
	{
		return left < right ? -1 : (left > right ? 1 : 0);
	}

	int compareRows(const RtosTaskRow& left, const RtosTaskRow& right, RtosTaskColumn column)
	{
		switch (column)
		{
			case RtosTaskColumn::Address:
				return signOf(left.tcbAddress, right.tcbAddress);

			case RtosTaskColumn::Priority:
				/* The rank, not the raw number: two kernels number their
				   priorities in opposite directions and the rank is what makes
				   the column mean the same thing for both. */
				return signOf(left.priorityRank, right.priorityRank);

			case RtosTaskColumn::State:
				return std::string(toString(left.state)).compare(toString(right.state));

			case RtosTaskColumn::StackSize:
				return signOf(left.stackBytes, right.stackBytes);

			case RtosTaskColumn::WaitingOn:
				return compareNames(left.waitingOn, right.waitingOn);

			case RtosTaskColumn::Name:
			default:
				return compareNames(left.name, right.name);
		}
	}
}  // namespace

std::vector<RtosTaskRow> RtosTableModel::taskRows(const RtosSnapshot& snapshot, const RtosTaskTableQuery& query)
{
	std::vector<RtosTaskRow> rows;

	rows.reserve(snapshot.tasks.size());

	for (const RtosTask& task : snapshot.tasks)
	{
		if (!matchesState(task.state, query.states))
			continue;

		/* A task with no priority does not pass a floor, and that is not the
		   same as failing one: nothing about it was read, so there is nothing
		   to compare against. */
		if (query.hasPriorityFloor && (!task.hasPriority || task.priorityRank < query.priorityFloor))
			continue;

		if (!containsIgnoringCase(task.name, query.nameSearch))
			continue;

		RtosTaskRow row;

		row.tcbAddress = task.tcbAddress;
		row.name = task.name;
		row.state = task.state;
		row.hasPriority = task.hasPriority;
		row.priority = task.rawPriority;
		row.basePriority = task.basePriority;
		row.priorityRank = task.priorityRank;
		row.hasStack = task.hasStack;
		row.stackBytes = (task.hasStack && task.stackTop > task.stackBase) ? task.stackTop - task.stackBase : 0;
		row.waitingOn = task.waitingOn;
		row.sourceList = task.sourceList;

		rows.push_back(std::move(row));
	}

	/* Stable, so that two tasks with the same key keep the order the round
	   found them in rather than swapping places on every redraw. */
	std::stable_sort(rows.begin(), rows.end(), [&query](const RtosTaskRow& left, const RtosTaskRow& right)
	{
		const int comparison = compareRows(left, right, query.sortColumn);

		return query.descending ? comparison > 0 : comparison < 0;
	});

	return rows;
}

std::vector<uint32_t> RtosTableModel::addressesOf(const std::vector<RtosTaskRow>& rows)
{
	std::vector<uint32_t> addresses;

	addresses.reserve(rows.size());

	for (const RtosTaskRow& row : rows)
		addresses.push_back(row.tcbAddress);

	return addresses;
}
