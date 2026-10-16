#include "RtosTableModel.hpp"

#include <algorithm>
#include <iterator>
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
		/* No case for a missing name here: a row with nothing read never meets
		   a row with something in the same sorted group. */
		return left.compare(right);
	}

	int signOf(int64_t left, int64_t right)
	{
		return left < right ? -1 : (left > right ? 1 : 0);
	}

	/*
	 * Whether the column's value was read for this row.
	 *
	 * A row with nothing read is not a small value and not a large one, so it
	 * cannot take part in the ordering. Sorting a column is a question - which
	 * task is closest to the end of its stack - and a row nobody has measured
	 * cannot answer it. Those rows are put at the end whichever way the column
	 * is sorted, so that the answer is not hidden behind the gaps.
	 */
	bool hasValueFor(const RtosTaskRow& row, RtosTaskColumn column)
	{
		switch (column)
		{
			case RtosTaskColumn::Name:
				return !row.name.empty();

			case RtosTaskColumn::Priority:
				return row.hasPriority;

			case RtosTaskColumn::StackSize:
				return row.hasStack;

			case RtosTaskColumn::StackWatermark:
				return row.hasStackWatermark;

			case RtosTaskColumn::Address:
			case RtosTaskColumn::State:
			case RtosTaskColumn::WaitingOn:
			default:
				/* Always present: an address comes from the list the task was
				   found on, and "waiting on nothing" is an answer rather than a
				   gap. */
				return true;
		}
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

			case RtosTaskColumn::StackWatermark:
				/* Least free first, so that the order answers the question the
				   column gets asked: which task is closest to the end of its
				   stack. A row with no reading never reaches this - it is put in
				   the group that trails, whichever way the column is sorted. */
				return signOf(left.stackFreeBytes, right.stackFreeBytes);


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
		row.hasStackWatermark = task.hasStackWatermark;
		row.stackFreeBytes = task.stackFreeBytes;
		row.stackWatermarkStackBytes = task.stackWatermarkStackBytes;
		row.stackWatermarkAgeSeconds = task.stackWatermarkAgeSeconds;
		row.stackWatermarkReachedCap = task.stackWatermarkReachedCap;

		row.waitingOn = task.waitingOn;
		row.sourceList = task.sourceList;

		rows.push_back(std::move(row));
	}

	/* Stable, so that two tasks with the same key keep the order the round
	   found them in rather than swapping places on every redraw. */
	const auto order = [&query](const RtosTaskRow& left, const RtosTaskRow& right)
	{
		const int comparison = compareRows(left, right, query.sortColumn);

		return query.descending ? comparison > 0 : comparison < 0;
	};

	/* Two groups rather than one comparison that would have to know the
	   direction: rows with a value for this column, sorted, then the rows
	   without one, sorted among themselves and left at the end. */
	std::vector<RtosTaskRow> read;
	std::vector<RtosTaskRow> notRead;

	read.reserve(rows.size());

	for (RtosTaskRow& row : rows)
	{
		if (hasValueFor(row, query.sortColumn))
			read.push_back(std::move(row));
		else
			notRead.push_back(std::move(row));
	}

	std::stable_sort(read.begin(), read.end(), order);
	std::stable_sort(notRead.begin(), notRead.end(), order);

	read.insert(read.end(), std::make_move_iterator(notRead.begin()), std::make_move_iterator(notRead.end()));

	return read;
}

std::vector<uint32_t> RtosTableModel::addressesOf(const std::vector<RtosTaskRow>& rows)
{
	std::vector<uint32_t> addresses;

	addresses.reserve(rows.size());

	for (const RtosTaskRow& row : rows)
		addresses.push_back(row.tcbAddress);

	return addresses;
}
