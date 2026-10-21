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
	bool stackIsBelowAlert(bool hasWatermark, uint32_t stackFreeBytes, uint32_t stackWatermarkStackBytes, uint32_t thresholdPercent)
	{
		if (thresholdPercent == 0 || !hasWatermark || stackWatermarkStackBytes == 0)
			return false;

		return (static_cast<uint64_t>(stackFreeBytes) * 100u) / stackWatermarkStackBytes < thresholdPercent;
	}

	bool stackIsBelowAlert(const RtosTask& task, uint32_t thresholdPercent)
	{
		return stackIsBelowAlert(task.hasStackWatermark, task.stackFreeBytes, task.stackWatermarkStackBytes, thresholdPercent);
	}

	int compareNames(const std::string& left, const std::string& right)
	{
		/* No case for a missing name here: a row with nothing read never meets
		   a row with something in the same sorted group. */
		return left.compare(right);
	}

	/*
	 * Whether the kernel lent this task a priority.
	 *
	 * One rule, three readers: the mode that keeps the anomalies, the summary
	 * count, and the order the table is put in. Written out three times it
	 * would sooner or later be written three slightly different ways, and the
	 * table would disagree with its own summary - the one disagreement a
	 * reader cannot see the cause of.
	 *
	 * Both flags have to be there. A zero priority is a real priority, so
	 * "nothing was read" cannot be spelled with a zero; without the flags every
	 * task whose control block was skipped would look as though it had been
	 * created at zero and inherited up to where it is.
	 */
	bool priorityIsInherited(bool hasPriority, bool hasBasePriority, uint32_t priority, uint32_t basePriority)
	{
		return hasPriority && hasBasePriority && priority != basePriority;
	}

	/*
	 * Whether the question being asked is about this task at all.
	 *
	 * Ahead of the filters rather than after them, because the two are not the
	 * same kind of condition: a filter narrows what the mode kept, and a row the
	 * mode drops was never part of the answer.
	 */
	bool keepsForMode(const RtosTaskRow& row, const RtosTaskTableQuery& query, const std::vector<uint32_t>& namedByFindings)
	{
		switch (query.mode)
		{
			case RtosTaskFilterMode::Blocked:
				return row.hasReading && row.state == RtosTaskState::Blocked;

			case RtosTaskFilterMode::HighShare:
				/* A task with no share is not a small one: nothing was measured
				   for it, and a column of them under a heading about the
				   busiest tasks would be the answer to a different question. */
				return row.hasReading && row.hasCpuShare && row.cpuSharePercent >= query.shareFloorPercent;

			case RtosTaskFilterMode::Anomalies:
				/* A row no round has read yet cannot be called healthy or
				   otherwise: this question is about the round, and the round
				   did not see it. */
				if (!row.hasReading)
					return false;

				if (row.stackIsLow)
					return true;

				if (priorityIsInherited(row.hasPriority, row.hasBasePriority, row.priority, row.basePriority))
					return true;

				/* The findings are the only source for a task that is ready and
				   being passed over: nothing in the round in front of you says
				   so, and a deadlock ring is a shape rather than a field. */
				return std::find(namedByFindings.begin(), namedByFindings.end(), row.tcbAddress) != namedByFindings.end();

			case RtosTaskFilterMode::All:
			default:
				return true;
		}
	}

	/* The filters that narrow inside the mode, applied to a row for the same
	   reason: a row without a reading can only be found by its name. */
	bool keepsForFilters(const RtosTaskRow& row, const RtosTaskTableQuery& query)
	{
		if (row.hasReading && !matchesState(row.state, query.states))
			return false;

		if (!row.hasReading && !query.states.empty())
			return false;

		/* A task with no priority does not pass a floor, and that is not the
		   same as failing one: nothing about it was read, so there is nothing
		   to compare against. */
		if (query.hasPriorityFloor && (!row.hasReading || !row.hasPriority || row.priorityRank < static_cast<int64_t>(query.priorityFloor)))
			return false;

		if (!containsIgnoringCase(row.name, query.nameSearch))
			return false;

		if (query.onlyStackAlerts && (!row.hasReading || !row.stackIsLow))
			return false;

		return true;
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
		/* A row the round did not read has no value in any column but the two
		   that are identity: its name and the address it is known by. */
		if (!row.hasReading)
			return column == RtosTaskColumn::Name || column == RtosTaskColumn::Address;

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

std::vector<RtosTaskRow> RtosTableModel::taskRows(const RtosSnapshot& snapshot, const RtosTaskTableQuery& query,
                                                  const std::vector<uint32_t>& namedByFindings)
{
	/* The same table, with a roster of one round: what an API client asking
	   "what is in this round" expects. The view passes the session's roster
	   instead, which is what keeps its rows from coming and going. */
	RtosTaskRoster round;

	round.observe(snapshot);

	return taskRows(round.entries(), snapshot, query, namedByFindings);
}

std::vector<RtosTaskRow> RtosTableModel::taskRows(const std::vector<RtosTaskRoster::Entry>& roster, const RtosSnapshot& snapshot,
                                                  const RtosTaskTableQuery& query, const std::vector<uint32_t>& namedByFindings)
{
	std::vector<RtosTaskRow> rows;

	rows.reserve(roster.size());

	for (const RtosTaskRoster::Entry& entry : roster)
	{
		const RtosTask* task = snapshot.find(entry.tcbAddress);

		RtosTaskRow row;

		row.tcbAddress = entry.tcbAddress;
		row.name = entry.name;

		/* The base priority is carried even when the round could not read the
		   task: it was set when the task was created, and it is what the lanes
		   are ordered by - a lane that moved because one round missed its task
		   would be worse than the gap it was reporting. */
		row.basePriority = entry.basePriority;
		row.hasBasePriority = entry.hasBasePriority;

		if (task == nullptr)
		{
			row.hasReading = false;
			row.sinceLastReadSeconds = snapshot.captureStartedAt > entry.lastSeenAt ? snapshot.captureStartedAt - entry.lastSeenAt : 0.0;
			row.sourceList = entry.sourceList;
		}
		else
		{
			row.state = task->state;
			/* A ready list states its own priority - it is one priority by
			   construction - so this needs no control block to be sound. See
			   `RtosTaskEnumerator::ListSource`. */
			row.hasPriority = task->hasPriority;
			row.priority = task->rawPriority;
			row.basePriority = task->basePriority;
			row.hasBasePriority = task->hasBasePriority;
			row.priorityRank = task->priorityRank;
			row.hasStack = task->hasStack;
			row.stackBytes = (task->hasStack && task->stackTop > task->stackBase) ? task->stackTop - task->stackBase : 0;
			row.hasCpuShare = task->hasCpuShare;
			row.cpuSharePercent = task->cpuSharePercent;
			row.hasStackWatermark = task->hasStackWatermark;
			row.stackFreeBytes = task->stackFreeBytes;
			row.stackWatermarkStackBytes = task->stackWatermarkStackBytes;
			row.stackWatermarkAgeSeconds = task->stackWatermarkAgeSeconds;
			row.stackWatermarkReachedCap = task->stackWatermarkReachedCap;
			row.waitingOn = task->waitingOn;
			row.sourceList = task->sourceList;

			/* Last, because it reads two of the fields above: computing it
			   while the row was being filled in would have compared zero
			   against the threshold and marked every row with a reading. */
			row.stackIsLow = stackIsBelowAlert(*task, query.stackAlertFreePercent);
		}

		if (!keepsForMode(row, query, namedByFindings))
			continue;

		if (!keepsForFilters(row, query))
			continue;

		rows.push_back(std::move(row));
	}

	/*
	 * The problem order: what a reader would fix first, first.
	 *
	 * Stack alerts, then tasks that are blocked with something to wait for,
	 * then priority inheritance, then rows with no reading of this round at
	 * all.
	 *
	 * Inside a rank the order is the base priority and then the name - not the
	 * share of the processor. The share is a measurement over a sliding window:
	 * it moves on every round, so ranking by it put the same rows in a
	 * different order twice a second, and the table appeared to shuffle itself
	 * while the reader was halfway down it. Base priority changes only when
	 * someone changes it, and ordering by it makes the table agree with the
	 * lanes, which are ordered the same way. "Which of these is busiest" is a
	 * column's question, and the column still answers it when clicked.
	 *
	 * The blocked rank is not a list of the anomalies - `Anomalies` does not
	 * keep every task waiting on an object, because in a healthy kernel that is
	 * most of them. It is here because the order is asked once and answered by
	 * everything that might matter, while the filter is asked again and again
	 * and cannot afford to be generous.
	 */
	const auto problemRank = [](const RtosTaskRow& row)
	{
		if (row.stackIsLow)
			return 0;

		if (row.state == RtosTaskState::Blocked && !row.waitingOn.empty())
			return 1;

		if (priorityIsInherited(row.hasPriority, row.hasBasePriority, row.priority, row.basePriority))
			return 2;

		/* "Not read this round" is not a problem and not a clean bill: it is
		   the one thing in the table the reader cannot judge for themselves, so
		   it goes last, together and out of the way. */
		if (!row.hasReading)
			return 4;

		return 3;
	};

	const auto problemOrder = [problemRank](const RtosTaskRow& left, const RtosTaskRow& right)
	{
		if (problemRank(left) != problemRank(right))
			return problemRank(left) < problemRank(right);

		/* A row whose base priority was never read goes after the ones that
		   have one, in both directions, for the same reason a missing value
		   goes last in a column sort: it is not a small priority. */
		if (left.hasBasePriority != right.hasBasePriority)
			return left.hasBasePriority;

		if (left.hasBasePriority && left.basePriority != right.basePriority)
			return left.basePriority > right.basePriority;

		return left.name < right.name;
	};

	/* Stable, so that two tasks with the same key keep the order the round
	   found them in rather than swapping places on every redraw. */
	const auto order = [&query](const RtosTaskRow& left, const RtosTaskRow& right)
	{
		const int comparison = compareRows(left, right, query.sortColumn);

		return query.descending ? comparison > 0 : comparison < 0;
	};

	if (query.problemFirst)
	{
		std::stable_sort(rows.begin(), rows.end(), problemOrder);

		return rows;
	}

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

		if (priorityIsInherited(task.hasPriority, task.hasBasePriority, task.rawPriority, task.basePriority))
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

std::vector<uint32_t> RtosTableModel::laneOrderOf(const std::vector<RtosTaskRow>& rows)
{
	std::vector<const RtosTaskRow*> ordered;
	ordered.reserve(rows.size());

	for (const RtosTaskRow& row : rows)
		ordered.push_back(&row);

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
	const auto lanePriority = [](const RtosTaskRow& row)
	{
		if (row.hasBasePriority)
			return static_cast<int64_t>(row.basePriority);

		if (row.hasPriority)
			return static_cast<int64_t>(row.priority);

		/* Nothing was read for it, which is not the lowest priority. */
		return static_cast<int64_t>(-1);
	};

	std::stable_sort(ordered.begin(), ordered.end(), [&](const RtosTaskRow* left, const RtosTaskRow* right)
	{
		if (lanePriority(*left) != lanePriority(*right))
			return lanePriority(*left) > lanePriority(*right);

		if (left->name != right->name)
			return left->name < right->name;

		return left->tcbAddress < right->tcbAddress;
	});

	std::vector<uint32_t> addresses;
	addresses.reserve(ordered.size());

	for (const RtosTaskRow* row : ordered)
		addresses.push_back(row->tcbAddress);

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
