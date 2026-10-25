#ifndef _RTOSTABLEMODEL_HPP
#define _RTOSTABLEMODEL_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "RtosSnapshot.hpp"
#include "RtosTaskRoster.hpp"

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
	/* Free bytes out of the stack's size, scanned on this side. */
	StackWatermark,

	/* Share of the time counted across all tasks. */
	CpuLoad,

	/* How many interrupts landed on this task. A count rather than a rate: the
	   column answers "who keeps being cut off", and the window it was counted
	   over is the one on screen. */
	Interrupted,


	/* Last, because the panel draws its headers by casting the column index
	   to this type: the order here is the order on screen. */
	WaitingOn,

	/* How many there are, so that the table is opened with this number instead
	   of a literal that can stay behind when a column is added. It happened:
	   nine `TableSetupColumn` calls against a table opened with eight, which
	   is an assertion and then an abort the moment the table was drawn. */
	Count,
};

/*
 * The question the table is being asked.
 *
 * Four of them, because four is what a reader arrives with: everything, what
 * is wrong, what is waiting, and what is taking the time. The filters below
 * narrow inside whichever one is asked, which is why this is a mode of its own
 * rather than one more checkbox among them - a row that the mode drops is not
 * a row whose other conditions failed, it is a row that this question is not
 * about.
 *
 * `Anomalies` is the one worth writing down, because it is the one the reader
 * will test against their own idea of "wrong". It keeps a row when the stack
 * is below the alert threshold, when the priority was raised by inheritance,
 * or when a finding names the task - the last being how a ready task that the
 * scheduler keeps passing over gets in, since nothing about the round in front
 * of you says so.
 *
 * It deliberately does not keep every task blocked on an object. In a healthy
 * kernel that is most of them, and a filter that answers "everything is wrong"
 * is a filter nobody uses twice.
 */
enum class RtosTaskFilterMode
{
	All,
	Anomalies,
	Blocked,
	HighShare,
};

struct RtosTaskTableQuery
{
	/* Which of the four questions this is. */
	RtosTaskFilterMode mode = RtosTaskFilterMode::All;

	/* What `HighShare` keeps: a share of the task time at or above this. A
	   judgement about one target, like the stack threshold, so it is a number
	   the reader can move rather than a constant. Zero keeps every task that
	   has a share at all. */
	double shareFloorPercent = 10.0;

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

	/* A task whose stack is this empty or less is marked. Zero turns the
	   marking off, and a row with no reading is never marked: there is nothing
	   to compare against, which is not the same as being comfortable. */
	uint32_t stackAlertFreePercent = 20;

	/* Only the rows the stack alert is on. Set from the summary line, where the
	   count is a thing to click rather than a number to read and then type. */
	bool onlyStackAlerts = false;

	/*
	 * Interrupts per task, keyed by TCB address.
	 *
	 * Passed in rather than worked out here: the answer needs the strip's
	 * running intervals, which the table does not have and should not. Null
	 * means nobody counted, and every row then shows a dash instead of a zero.
	 */
	const std::map<uint32_t, uint32_t>* isrByTask = nullptr;

	RtosTaskColumn sortColumn = RtosTaskColumn::Name;
	bool descending = false;

	/*
	 * The default order is by problem, not by name.
	 *
	 * A table sorted by name makes the reader do the sorting: the task with
	 * 2 % of its stack left sits wherever its name falls, and finding it means
	 * reading every row. Sorted by problem it is the first row. Clicking a
	 * column header turns this off - that is the reader saying which order
	 * they want, and it stays off until the default is asked for again.
	 */
	bool problemFirst = true;
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

	/*
	 * Whether the round this table was built from read this task at all.
	 *
	 * False is not "every field happened to be missing": it is a task the
	 * session has seen and the newest round cannot. The row stays - the set of
	 * rows is the set of tasks the session knows, not the set one round
	 * happened to catch - and everything in it is shown as missing. Deleting
	 * the row instead would say the task had gone, which is a stronger claim
	 * than a short walk of a list supports.
	 */
	bool hasReading = true;

	/* How long before this round the task was last read, in seconds. Only
	   meaningful when `hasReading` is false. */
	double sinceLastReadSeconds = 0.0;

	bool hasPriority = false;
	uint32_t priority = 0;

	/* The priority the task was created with, and whether it was read. Zero is
	   a real priority, so "no reading" has to be its own flag or every task
	   without a control block looks like it was created at zero and has been
	   inherited up to where it is. */
	uint32_t basePriority = 0;
	bool hasBasePriority = false;
	int64_t priorityRank = 0;

	bool hasStack = false;
	uint32_t stackBytes = 0;

	/* What the stack scan found, and what the reading is worth: the moment it
	   was taken at, since the sweep reads a few tasks a round, and whether the
	   scan stopped at its cap and so gives a lower bound rather than a count.
	   All of it is kept so the panel can say how old a number is instead of
	   presenting every row as equally fresh. */
	bool hasStackWatermark = false;
	uint32_t stackFreeBytes = 0;
	uint32_t stackWatermarkStackBytes = 0;
	double stackWatermarkAgeSeconds = 0.0;
	bool stackWatermarkReachedCap = false;

	/* The stack the task has touched, and its share of the whole. Worked out
	   here rather than in the panel: the panel is not built into the test
	   binary, so a number worked out there is a number nothing checks. */
	uint32_t stackUsedBytes() const
	{
		return stackWatermarkStackBytes > stackFreeBytes ? stackWatermarkStackBytes - stackFreeBytes : 0;
	}

	uint32_t stackUsedPercent() const
	{
		if (stackWatermarkStackBytes == 0)
			return 0;

		return static_cast<uint32_t>((static_cast<uint64_t>(stackUsedBytes()) * 100u) / stackWatermarkStackBytes);
	}

	/* The share of the stack still holding the fill pattern, which is the
	   share the alert is about: a stack that is nearly full is the finding,
	   and it reads the same way round as the mark that flags it. */
	uint32_t stackFreePercent() const
	{
		if (stackWatermarkStackBytes == 0)
			return 0;

		return static_cast<uint32_t>((static_cast<uint64_t>(stackFreeBytes) * 100u) / stackWatermarkStackBytes);
	}

	/* Whether this row crossed the threshold the query carried. Decided here
	   rather than in the panel, so that the rule is one the tests can hold. */
	bool stackIsLow = false;

	/* This task's share of the time the kernel counted, and whether there is
	   one. A share needs two rounds, so on the first round of a session no row
	   has one - which is a gap rather than a task that used no time. */
	bool hasCpuShare = false;
	double cpuSharePercent = 0.0;

	/* Interrupts that landed on this task while it was running. `hasIsrCount`
	   is false when the source carries no interrupts at all - an eight byte
	   ring, or a target whose firmware has none - and that is a dash, not a
	   zero: "nobody counted" and "it was interrupted no times" are different
	   findings and the second is worth acting on. */
	bool hasIsrCount = false;
	uint32_t isrCount = 0;

	/* The name of the object it is waiting on, and empty when it is waiting on
	   none. Unlike the fields above, an empty one here is not missing data -
	   it is the answer for every task that is ready, delayed or suspended. */
	std::string waitingOn;

	/* The name of the list it was found on, for a reader who wants to know
	   where a state came from without knowing the kernel's vocabulary. */
	std::string sourceList;
};

/*
 * What the reader had set up, kept between runs.
 *
 * Sort order and filters are judgements about one target, made once and wanted
 * back; so is the timeline's window and whether it follows the plot cursors.
 * Plain data, so that the settings file can hold it without knowing what an
 * RTOS is.
 */
struct RtosViewSettings
{
	/* The order the table opens in: a column, or the problem order. Fixed by
	   the column by default - the problem order re-ranks rows as the target
	   changes, which is the right answer to "what should I look at" and the
	   wrong one to watch a running system with. */
	uint32_t sortColumn = 0;
	bool sortDescending = false;
	bool problemFirst = false;
	bool hasPriorityFloor = false;
	uint32_t priorityFloor = 0;
	bool onlyStackAlerts = false;
	uint32_t stackAlertFreePercent = 20;
	float timelineWindowSeconds = 10.0f;
	bool followPlotCursors = true;

	/* Whether the detail block under the table is open. */
	bool selectedTaskOpen = true;

	/* Whether the strip's rows follow the table's order rather than the stable
	   one. Off by default: a table sorted by a number that changes every round
	   would make every lane move every round. */
	bool timelineFollowsTable = false;

	/* Which of the four questions the table is answering, as the number of the
	   mode, and the floor `HighShare` uses. Stored as numbers rather than as
	   names for the same reason the theme is stored as one: a value this build
	   does not recognise has to be recognisable as such. */
	uint32_t filterMode = 0;
	double shareFloorPercent = 10.0;
};

class RtosTableModel
{
   public:
	/*
	 * The rows the query keeps, in the order it asks for.
	 *
	 * `namedByFindings` is the tasks the round's findings named. They come
	 * from `RtosDiagnostics`, which needs the history and therefore cannot be
	 * called from here; what arrives is the addresses, and the only mode that
	 * looks at them is `Anomalies`.
	 */
	static std::vector<RtosTaskRow> taskRows(const RtosSnapshot& snapshot, const RtosTaskTableQuery& query,
	                                         const std::vector<uint32_t>& namedByFindings = {});

	/*
	 * The table as the view shows it: a row for every task the session has
	 * seen, filled from the round where the round read it and marked where it
	 * did not.
	 *
	 * The single-round overload above builds the same thing from a roster of
	 * one round, which is what an API client asking "what is in this round"
	 * wants; the view wants the session.
	 */
	static std::vector<RtosTaskRow> taskRows(const std::vector<RtosTaskRoster::Entry>& roster, const RtosSnapshot& snapshot,
	                                         const RtosTaskTableQuery& query, const std::vector<uint32_t>& namedByFindings = {});

	/*
	 * The task numbers a row set holds, in order.
	 *
	 * Used to decide whether the table has to be rebuilt: the rows change on
	 * every round - a state, a stack size - while the set of tasks usually
	 * does not, and redrawing identical rows at the sampling rate is work with
	 * nothing to show for it.
	 */
	static std::vector<uint32_t> addressesOf(const std::vector<RtosTaskRow>& rows);

	/*
	 * The order the timeline draws its rows in: highest priority first, then by
	 * name, then by address.
	 *
	 * Not the table's order, which is whatever the reader sorted it by, and not
	 * the order the round found them in, which changes every round as tasks
	 * move between lists. A lane that moves under the reader is a lane they
	 * cannot follow, so the order has to come from something the target does
	 * not change from round to round.
	 *
	 * It takes rows rather than the snapshot so that the strip holds the same
	 * tasks the table does. A filter that narrowed the table to three rows and
	 * left fourteen lanes would answer half the question: which tasks are worth
	 * watching is one question, and the lanes are where you watch them.
	 */
	static std::vector<uint32_t> laneOrderOf(const std::vector<RtosTaskRow>& rows);

	/*
	 * The state's name as the interface writes it.
	 *
	 * `toString` stays the name the API reports: a caller reading JSON needs one
	 * spelling whatever language the interface happens to be in. This one is for
	 * the screen, and it is the only place a state is turned into words - the
	 * table's column and the timeline's counts both come through here, so the
	 * two cannot end up calling the same state two different things.
	 *
	 * A test drives every state through the translation table, so a missing
	 * entry fails there rather than leaving an English word in the Chinese
	 * interface, which is the kind of thing nobody notices until a user does.
	 */
	static const char* taskStateLabel(RtosTaskState state);

	/*
	 * The numbers a reader wants before anything else: how many tasks there
	 * are, what they are doing, and which of the two findings worth acting on
	 * are present.
	 *
	 * Counted over the whole round rather than over the rows a filter happens
	 * to let through: a summary that answers "what is wrong" cannot itself be
	 * narrowed by a question about something else. A task with no state counts
	 * as unread, which is what it is.
	 */
	struct Summary
	{
		uint32_t tasks = 0;
		uint32_t running = 0;
		uint32_t ready = 0;
		uint32_t blocked = 0;
		uint32_t suspended = 0;
		uint32_t unread = 0;

		/* Stacks below the alert threshold, and tasks whose current priority
		   has been raised above their base one - inheritance, in other words,
		   which is the visible sign of a task waiting on a lock. */
		uint32_t stackAlerts = 0;
		uint32_t inherited = 0;
	};

	static Summary summaryOf(const RtosSnapshot& snapshot, uint32_t stackAlertFreePercent);
};

#endif
