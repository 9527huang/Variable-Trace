#ifndef _RTOSTABLEMODEL_HPP
#define _RTOSTABLEMODEL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "RtosSnapshot.hpp"

/*
 * What the task table shows, and in what order.
 *
 * Sorting and filtering live here rather than in the panel because they are
 * decisions about the data and not about pixels: which rows are shown, in what
 * order, and what a row says when a field was not read. The panel draws; this
 * decides what it draws.
 *
 * The split is also what makes any of it testable. The panels are not built
 * into the test binary - only the reasoning behind them is - so a decision
 * taken inside a panel is a decision nothing checks.
 */
enum class RtosTaskColumn
{
	Name,
	Address,
	Priority,
	State,
	StackSize,

	/* Last, because the panel draws its headers by casting the column index
	   to this type: the order here is the order on screen. */
	WaitingOn,
};

struct RtosTaskTableQuery
{
	/* Empty means every state. */
	std::vector<RtosTaskState> states;

	/*
	 * Rows whose priority ranks below this are left out.
	 *
	 * A task with no priority does not pass a floor, and that is not the same
	 * as failing it: it was seen on a list that does not carry the priority,
	 * so there is nothing to compare. Filtering it out silently would hide
	 * exactly the tasks whose state is hardest to account for.
	 */
	bool hasPriorityFloor = false;
	int64_t priorityFloor = 0;

	/* Case insensitive part of the name. Empty means no search. */
	std::string nameSearch;

	RtosTaskColumn sortColumn = RtosTaskColumn::Name;
	bool descending = false;
};

/*
 * One row of the table.
 *
 * Every field that a round may not have read carries a flag beside it. A zero
 * shown where a value was never read is worse than a blank, because a blank
 * says "not known" and a zero says "zero" - and the second is a fact about the
 * target that nobody checked.
 */
struct RtosTaskRow
{
	uint32_t tcbAddress = 0;
	std::string name;
	RtosTaskState state = RtosTaskState::Unknown;

	bool hasPriority = false;
	uint32_t priority = 0;
	uint32_t basePriority = 0;
	int64_t priorityRank = 0;

	bool hasStack = false;
	uint32_t stackBytes = 0;

	/* The name of the object it is waiting on, and empty when it is waiting on
	   none. Unlike the fields above, an empty one here is not missing data -
	   it is the answer for every task that is ready, delayed or suspended. */
	std::string waitingOn;

	/* The name of the list it was found on, for a reader who wants to know
	   where a state came from without knowing the kernel's vocabulary. */
	std::string sourceList;
};

class RtosTableModel
{
   public:
	static std::vector<RtosTaskRow> taskRows(const RtosSnapshot& snapshot, const RtosTaskTableQuery& query);

	/*
	 * The task numbers a row set holds, in order.
	 *
	 * Used to decide whether the table has to be rebuilt: the rows change on
	 * every round - a state, a stack size - while the set of tasks usually
	 * does not, and redrawing identical rows at the sampling rate is work with
	 * nothing to show for it.
	 */
	static std::vector<uint32_t> addressesOf(const std::vector<RtosTaskRow>& rows);
};

#endif
