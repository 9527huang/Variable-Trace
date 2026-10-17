#include "RtosTableModel.hpp"

#include "I18n.hpp"

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

	/*
	 * Whether a task's stack is below the alert threshold.
	 *
	 * One definition for the row's mark, the filter the summary sets and the
	 * count the summary shows. Three copies would drift apart, and the table
	 * would then disagree with its own summary - which is the one kind of
	 * disagreement a reader cannot see the cause of.
	 */
	bool stackIsBelowAlert(const RtosTask& task, uint32_t thresholdPercent)
	{
		if (thresholdPercent == 0 || !task.hasStackWatermark || task.stackWatermarkStackBytes == 0)
			return false;

		return (static_cast<uint64_t>(task.stackFreeBytes) * 100u) / task.stackWatermarkStackBytes < thresholdPercent;
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

	int compareFractions(double left, double right)
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

			case RtosTaskColumn::CpuLoad:
				return row.hasCpuShare;

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

			case RtosTaskColumn::CpuLoad:
				/* Busiest first when the column is sorted downwards, which is the
				   question it gets asked. A row with no share never reaches this:
				   it is put in the group that trails either way. */
				return compareFractions(left.cpuSharePercent, right.cpuSharePercent);

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

		if (query.onlyStackAlerts && !stackIsBelowAlert(task, query.stackAlertFreePercent))
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
		row.hasCpuShare = task.hasCpuShare;
		row.cpuSharePercent = task.cpuSharePercent;
		row.hasStackWatermark = task.hasStackWatermark;
		row.stackFreeBytes = task.stackFreeBytes;
		row.stackWatermarkStackBytes = task.stackWatermarkStackBytes;
		row.stackWatermarkAgeSeconds = task.stackWatermarkAgeSeconds;
		row.stackWatermarkReachedCap = task.stackWatermarkReachedCap;

		row.waitingOn = task.waitingOn;
		row.sourceList = task.sourceList;

		/* Last, because it reads two of the fields above: computing it while
		   the row was being filled in would have compared zero against the
		   threshold and marked every row with a reading. */
		row.stackIsLow = stackIsBelowAlert(task, query.stackAlertFreePercent);

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

RtosTableModel::Summary RtosTableModel::summaryOf(const RtosSnapshot& snapshot, uint32_t stackAlertFreePercent)
{
	Summary summary;

	for (const RtosTask& task : snapshot.tasks)
	{
		summary.tasks++;

		switch (task.state)
		{
			case RtosTaskState::Running:
				summary.running++;
				break;

			case RtosTaskState::Ready:
			case RtosTaskState::PendingReady:
				summary.ready++;
				break;

			case RtosTaskState::Blocked:
				summary.blocked++;
				break;

			case RtosTaskState::Suspended:
				summary.suspended++;
				break;

			case RtosTaskState::Deleted:
			case RtosTaskState::Unknown:
			default:
				summary.unread++;
				break;
		}

		/* The same judgement the row makes, so that the count and the marks in
		   the table say the same thing. */
		if (stackIsBelowAlert(task, stackAlertFreePercent))
			summary.stackAlerts++;

		if (task.hasBasePriority && task.hasPriority && task.rawPriority != task.basePriority)
			summary.inherited++;
	}

	return summary;
}

const char* RtosTableModel::taskStateLabel(RtosTaskState state)
{
	switch (state)
	{
		case RtosTaskState::Running:
			return I18n::tr("running");

		case RtosTaskState::Ready:
			return I18n::tr("ready");

		case RtosTaskState::Blocked:
			return I18n::tr("blocked");

		case RtosTaskState::Suspended:
			return I18n::tr("suspended");

		case RtosTaskState::PendingReady:
			return I18n::tr("pending ready");

		case RtosTaskState::Deleted:
			return I18n::tr("deleted");

		case RtosTaskState::Unknown:
		default:
			return I18n::tr("unknown");
	}
}

std::vector<uint32_t> RtosTableModel::laneOrder(const RtosSnapshot& snapshot)
{
	std::vector<const RtosTask*> ordered;
	ordered.reserve(snapshot.tasks.size());

	for (const RtosTask& task : snapshot.tasks)
		ordered.push_back(&task);

	/*
	 * The base priority, where the target keeps one.
	 *
	 * A task holding a mutex has its current priority raised while another waits
	 * for it, so ordering by the current priority makes lanes jump from round to
	 * round - exactly when a reader is looking at them, because inheritance
	 * happens when they are looking. The base value only changes when someone
	 * sets it.
	 *
	 * A second kernel will need this to be a rank of the base priority rather
	 * than its raw number: Zephyr numbers the other way round. The rank the
	 * enumeration carries is of the current priority, which is why it is not
	 * used here.
	 */
	const auto lanePriority = [](const RtosTask& task)
	{
		if (task.hasBasePriority)
			return static_cast<int64_t>(task.basePriority);

		if (task.hasPriority)
			return static_cast<int64_t>(task.rawPriority);

		/* Nothing was read for it, which is not the lowest priority. */
		return static_cast<int64_t>(-1);
	};

	std::stable_sort(ordered.begin(), ordered.end(), [&](const RtosTask* left, const RtosTask* right)
	{
		if (lanePriority(*left) != lanePriority(*right))
			return lanePriority(*left) > lanePriority(*right);

		if (left->name != right->name)
			return left->name < right->name;

		return left->tcbAddress < right->tcbAddress;
	});

	std::vector<uint32_t> addresses;
	addresses.reserve(ordered.size());

	for (const RtosTask* task : ordered)
		addresses.push_back(task->tcbAddress);

	return addresses;
}

std::vector<uint32_t> RtosTableModel::addressesOf(const std::vector<RtosTaskRow>& rows)
{
	std::vector<uint32_t> addresses;

	addresses.reserve(rows.size());

	for (const RtosTaskRow& row : rows)
		addresses.push_back(row.tcbAddress);

	return addresses;
}
