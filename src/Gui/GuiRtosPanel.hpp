#ifndef _GUIRTOSPANEL_HPP
#define _GUIRTOSPANEL_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "GuiHelper.hpp"
#include "GuiRtosPanelTimeline.hpp"
#include "GuiRtosSelection.hpp"
#include "RtosDiagnostics.hpp"
#include "I18n.hpp"
#include "RtosCsvExport.hpp"
#include "RtosEventQuery.hpp"
#include "RtosModel.hpp"
#include "RtosSyncEnumerator.hpp"
#include "RtosTableModel.hpp"
#include "IFileHandler.hpp"
#include "imgui.h"

/*
 * The task view.
 *
 * It is one of the three top level views, alongside the variable view and the
 * trace view, and not a dialog belonging to either. The window it lives in is
 * opened by the view itself; this draws what goes in it.
 *
 * It draws and nothing else. Which rows exist, in what order, and what a row
 * says when a field was never read are decided by `RtosTableModel`, which is
 * built into the test binary; this file is not, so anything decided here is a
 * decision nothing checks.
 *
 * The columns the plan asks for and this table does not show are missing for a
 * reason worth writing down. The kernel keeps no per task switch count at all,
 * and a column filled with zeroes would be a claim about the target rather
 * than a gap in the reading.
 *
 * The stack columns are here because they ask nothing of the target beyond
 * what makes them measurable anywhere: the range comes out of the control
 * block, and how much of it is used is worked out on this side from the
 * pattern a new stack is filled with.
 */
class GuiRtosPanel
{
   public:
	static void draw(const std::shared_ptr<RtosModel>& model, const std::string& explanation, uint32_t stackAlertFreePercent, RtosViewSettings& view,
	                 IFileHandler* fileHandler)
	{
		/* What the reader had set up is taken once, from the settings file, so
		   that a run does not change under a number someone is moving in a box -
		   and then written back every frame, so that what they set up in this
		   run is what the next one opens with. */
		if (!stackAlertSeeded)
		{
			query.sortColumn = static_cast<RtosTaskColumn>(view.sortColumn);
			query.descending = view.sortDescending;
			query.problemFirst = view.problemFirst;
			query.hasPriorityFloor = view.hasPriorityFloor;
			query.priorityFloor = view.priorityFloor;
			query.onlyStackAlerts = view.onlyStackAlerts;
			query.stackAlertFreePercent = stackAlertFreePercent;
			query.mode = filterModeFrom(view.filterMode);
			query.shareFloorPercent = view.shareFloorPercent;

			selectedTaskOpen = view.selectedTaskOpen;
			GuiRtosPanelTimeline::setWindowSeconds(view.timelineWindowSeconds);
			GuiRtosPanelTimeline::setFollowPlotCursors(view.followPlotCursors);
			GuiRtosPanelTimeline::setFollowTableOrder(view.timelineFollowsTable);

			stackAlertSeeded = true;
		}

		view.sortColumn = static_cast<uint32_t>(query.sortColumn);
		view.sortDescending = query.descending;
		view.problemFirst = query.problemFirst;
		view.hasPriorityFloor = query.hasPriorityFloor;
		view.priorityFloor = query.priorityFloor;
		view.onlyStackAlerts = query.onlyStackAlerts;
		view.filterMode = static_cast<uint32_t>(query.mode);
		view.shareFloorPercent = query.shareFloorPercent;
		view.selectedTaskOpen = selectedTaskOpen;
		view.timelineWindowSeconds = GuiRtosPanelTimeline::getWindowSeconds();
		view.followPlotCursors = GuiRtosPanelTimeline::getFollowPlotCursors();
		view.timelineFollowsTable = GuiRtosPanelTimeline::getFollowTableOrder();

		if (model == nullptr)
		{
			/* The reason is shown when there is one. An empty panel that says
			   only "nothing here" leaves the reader to work out whether the
			   switch is off, the acquisition is stopped, or the project open
			   is not the one built with the kernel in it - and the last of
			   those is the one nobody guesses. */
			ImGui::TextUnformatted(I18n::tr("No task data yet."));

			if (!explanation.empty())
				GuiHelper::drawSecondaryTextWrapped("%s", explanation.c_str());
			else
				GuiHelper::drawSecondaryTextWrapped("%s", I18n::tr("The task view reads its rounds while an acquisition is running."));

			return;
		}

		/* Before the round: where the round came from is the first thing to know
		   about it, and it is the only place a recording is started or left. */
		drawSourceLine(model, fileHandler);

		const std::shared_ptr<const RtosSnapshot> snapshot = model->current();

		if (snapshot == nullptr)
		{
			ImGui::TextUnformatted(I18n::tr("No round has been published yet."));

			return;
		}

		drawConsistencyLine(*snapshot);
		drawStackScanNotice(*snapshot);
		drawCpuLoadNotice(*snapshot);

		/* The findings are worked out once for the whole round and kept, because
		   three readers want them: this half quotes them, the table's anomaly
		   mode keeps the tasks they name, and the detail block says which of
		   them the selected task is caught in. They are the summary's source for
		   the longest wait too, so a summary and a findings block cannot
		   disagree about which wait that is. */
		const std::vector<RtosDiagnostics::Finding>& findings = findingsFor(*model, snapshot);

		fixedRows(2.0f, "##rtosSummary", [&] { drawSummary(*snapshot, findings); });

		/*
		 * Always, and always the same height.
		 *
		 * It used to be skipped when there was nothing to report, on the
		 * reasoning that a block which says "nothing is wrong" every round
		 * teaches the reader to skip it. What that missed is that the number of
		 * findings changes while the target runs, and a block that comes and
		 * goes moves the filters and the table under the reader's eye. The
		 * section keeps its room and says so in words when it is empty.
		 */
		drawDiagnostics(findings);

		const std::vector<RtosTaskRow> rows = drawFilters(*model, snapshot, addressesNamedBy(findings));

		/*
		 * One line, three things it can say, and never more than one line.
		 *
		 * A table with no rows and no word about why reads as a broken panel
		 * rather than as an answer, and a table that has rows still owes the
		 * reader which order they are in - the header shows no arrow while the
		 * problem order is in force, and a table with no arrow on any column
		 * looks like a table that forgot to sort.
		 */
		if (rows.empty())
			GuiHelper::drawSecondaryText("%s", emptyWords(*snapshot));
		else if (query.problemFirst)
			GuiHelper::drawSecondaryText("%s", I18n::tr("sorted by problem: stack alerts, blocked, inherited, then by base priority"));
		else
		{
			GuiHelper::drawSecondaryText("%s", I18n::tr("sorted by the column in bold"));

			ImGui::SameLine(0.0f, 8.0f * GuiHelper::contentScale);

			if (ImGui::SmallButton(I18n::tr("back to problem order")))
			{
				query.problemFirst = true;
				query.descending = false;
			}
		}

		/*
		 * The detail sits under the table, on the side of "now".
		 *
		 * It is a block about one task as it is in this round, which is what
		 * the table is about; the right hand half is about time and is read
		 * with the timeline. The space it takes comes off the table, which is
		 * the one block here that can be given less - and folding it gives the
		 * table all of it back.
		 */
		const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
		const float room = ImGui::GetContentRegionAvail().y;
		const float folded = lineHeight + 4.0f * GuiHelper::contentScale;
		const float reserve = selectedTaskOpen ? std::clamp(room * 0.34f, folded, 12.0f * lineHeight) : folded;
		/* The table takes what its rows need, not what is left.
		 *
		 * Filling the pane left a column of empty table under the last row and
		 * pushed the detail block to the bottom of the window, half a pane away
		 * from the row it was describing. Sized to its rows, the block sits
		 * directly under the last one - which is where a reader looking at a
		 * row expects the detail of that row to be. */
		const float tableContent = static_cast<float>(rows.size() + 2) * lineHeight + 4.0f * GuiHelper::contentScale;
		const float tableHeight = std::clamp(tableContent, 8.0f * lineHeight, std::max(room - reserve - 6.0f * GuiHelper::contentScale, 8.0f * lineHeight));

		drawTable(snapshot, rows, tableHeight);

		drawSelectedTaskSection(*model, *snapshot, findings);
	}

	/*
	 * The right hand half of the task view: what the tasks did over time.
	 *
	 * The table says what is true now and the timeline says how it got there,
	 * so the two are read together. Side by side they are read together
	 * without either being squeezed: stacked in one column narrow enough to
	 * fit the settings, the timeline lost its labels and the table lost its
	 * columns.
	 *
	 * It draws in a window of its own, which is why it is a method of its own
	 * and not the tail of `draw`.
	 */
	static void drawAnalysis(const std::shared_ptr<RtosModel>& model, const std::string& explanation, const std::vector<double>& plotCursors)
	{
		/* A hovered row is a fact about this frame of the table, and the table
		   may not have been drawn at all - the reader can be looking at
		   another view with this one still open beside it. A stale address
		   would leave a lane lit with nothing pointing at it. */
		if (!selection().tableDrewThisFrame)
			selection().hoveredTask = 0;

		selection().tableDrewThisFrame = false;

		if (model == nullptr)
		{
			ImGui::TextUnformatted(I18n::tr("No task data yet."));

			if (!explanation.empty())
				ImGui::TextDisabled("%s", explanation.c_str());
			else
				ImGui::TextDisabled("%s", I18n::tr("The task view reads its rounds while an acquisition is running."));

			return;
		}

		const std::shared_ptr<const RtosSnapshot> snapshot = model->current();

		if (snapshot == nullptr)
		{
			ImGui::TextUnformatted(I18n::tr("No round has been published yet."));

			return;
		}

		/* The same round the table was built from, and the same findings: the
		   strip has to hold the tasks the table kept, or the two halves of the
		   view would be answering different questions. */
		const std::vector<RtosDiagnostics::Finding>& findings = findingsFor(*model, snapshot);
		const std::vector<RtosTaskRow> rows = currentRows(*model, *snapshot, addressesNamedBy(findings));

		/* Which order the rows go in: the stable one, or the one the reader
		   sorted the table to. The stable order exists because a lane that
		   moves is a lane that cannot be followed; the table's order exists
		   because a reader comparing a row with a lane wants them adjacent. */
		const std::vector<uint32_t> lanes = GuiRtosPanelTimeline::getFollowTableOrder() ? RtosTableModel::addressesOf(rows)
		                                                                               : RtosTableModel::laneOrderOf(rows);

		GuiRtosPanelTimeline::draw(*model, *snapshot, plotCursors, rows, lanes, model->rosterSize(), selection());

		/*
		 * In a recording the strip is also the scrubber.
		 *
		 * A live view always shows the newest round, because that is the only
		 * one a probe can give. A file holds every round, and the question
		 * "what was it doing then" is the whole reason for keeping them - so
		 * the pinned moment decides which round the table is built from.
		 *
		 * Only when something is pinned. Opening a file shows its last round
		 * and nothing moves the round after that unless the reader asks: a view
		 * that snapped back to the newest round every frame would undo both a
		 * pin and anything else that had moved it, and the strip's own window
		 * would be unreadable the moment it was dragged back in time.
		 *
		 * Asked after the strip has drawn, because the click is handled there:
		 * the round follows the click on the next frame, one frame behind every
		 * other click in a window.
		 */
		if (model->isPlayback() && GuiRtosPanelTimeline::getPinned())
		{
			const size_t wanted = model->nearestPlaybackRound(GuiRtosPanelTimeline::getPinnedAt());

			if (wanted != model->playbackInfo().current)
				model->showPlaybackRound(wanted);
		}

		/* Under the strip, and in the same window: a task that is waiting is
		   waiting for one of these, so the two are read together. */
		/* The counts are the tallest each block gets, not the counts it happens
		   to need this frame: eight rows for the objects - a heading, the
		   table's own header and four rows of them - and five for the pinned
		   moment, which is a heading and three rows whether or not a moment has
		   been pinned. What they do not use is left blank. */
		fixedRows(8.0f, "##rtosSyncObjects", [&] { drawSyncObjects(*snapshot); });
		fixedRows(5.0f, "##rtosPinnedMoment", [&] { drawPinnedMoment(*model, *snapshot); });

		/* The last block, and the one that takes what is left: a list is read
		   by scrolling it, so a short window costs rows rather than frames. */
		drawEvents(*model, rows);

		/*
		 * The detail takes what is left rather than a fixed number of rows.
		 *
		 * It is the last block in the pane, and the only one that can hold more
		 * than fits: a task with everything read fills twelve lines, and on a
		 * window that is not tall enough those twelve would push the last two -
		 * Waiting on and Last ran - past the bottom edge, where no amount of
		 * scrolling on the reader's part would reach them. Given the remaining
		 * space and its own scrollbar, the block still does not move, and
		 * nothing is unreachable.
		 */
	}

	/*
	 * What the snapshot is and is not.
	 *
	 * The round walks the kernel's lists one after another while the target
	 * moves tasks between them, so its parts were not all taken at the same
	 * instant. That is not a defect to hide - it is the honest answer to "when
	 * was this true", and it belongs on screen rather than in a log.
	 *
	 * It is drawn plainly, though, while the count is within what a round can
	 * expect of itself. A running target moves a few tasks between lists every
	 * time, so a warning that is on in every round is a warning nobody reads -
	 * and the round that really did go wrong is then the one nobody notices.
	 * The colour comes out only above the limit the snapshot states, and the
	 * sentence says the same thing in words beside it, because colour is not
	 * something every reader has.
	 */
	static void drawConsistencyLine(const RtosSnapshot& snapshot)
	{
		const int places = static_cast<int>(snapshot.inconsistencies.size());
		const double spanMs = snapshot.captureSpanSeconds * 1000.0;
		const bool shaky = snapshot.consistencyIsShaky();

		if (shaky)
			ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::orangeLight());

		if (places == 0)
			ImGui::Text(I18n::tr("Read over %.1f ms; every kernel list held together."), spanMs);
		else if (shaky)
			ImGui::Text(I18n::tr("Read over %.1f ms; %d place(s) caught the target moving, so the parts of this table are not all from one moment."), spanMs, places);
		else
			ImGui::Text(I18n::tr("Read over %.1f ms; %d place(s) caught the target moving."), spanMs, places);

		if (shaky)
			ImGui::PopStyleColor();

		if (places > 0 && ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();

			for (const std::string& note : snapshot.inconsistencies)
				ImGui::TextUnformatted(note.c_str());

			ImGui::EndTooltip();
		}

		ImGui::Separator();
	}

	/*
	 * Why the stack column is empty, when it is empty because of the target.
	 *
	 * A kernel that never fills its stacks leaves nothing to measure: every byte
	 * was written at some point and no boundary between used and unused exists.
	 * The scan then starts on a byte that is not the pattern and reports zero
	 * free bytes, which is exactly what a stack that is full to the brim
	 * reports. The two cannot be told apart from one stack, so nothing is shown
	 * until the pattern has been seen somewhere - and while nothing is shown,
	 * this says which option would make the numbers possible.
	 */
	static void drawStackScanNotice(const RtosSnapshot& snapshot)
	{
		if (!snapshot.stackScan.enabled || snapshot.stackScan.fillSeen)
			return;

		GuiHelper::drawSecondaryTextWrapped("%s", I18n::tr("The target does not fill its stacks, so how much of one is used cannot be worked out. FreeRTOS fills them when configCHECK_FOR_STACK_OVERFLOW is above 1, or configUSE_TRACE_FACILITY, or INCLUDE_uxTaskGetStackHighWaterMark, or INCLUDE_uxTaskGetStackHighWaterMark2 is set."));

		ImGui::Separator();
	}

	/*
	 * Why the CPU column is empty, when it is empty because of the target.
	 *
	 * A kernel counts per task run time only when it was built to, and a counter
	 * that never moves gives a column of zeroes - which reads as a system with
	 * nothing to do rather than as a reading that could not be taken. Both are
	 * said here instead of being left to be guessed at from a column of dashes.
	 *
	 * A round with no share for anyone and no movement is the first round, where
	 * there is nothing to compare against yet. That fixes itself in one round,
	 * so nothing is said about it.
	 */
	static void drawCpuLoadNotice(const RtosSnapshot& snapshot)
	{
		if (!snapshot.cpuLoad.supported)
		{
			GuiHelper::drawSecondaryTextWrapped("%s", I18n::tr("The target was not built with run time statistics (configGENERATE_RUN_TIME_STATS), so how much of the time each task used cannot be worked out."));

			ImGui::Separator();

			return;
		}

		if (snapshot.cpuLoad.tasksMeasured > 0 && !snapshot.cpuLoad.counting)
		{
			GuiHelper::drawSecondaryTextWrapped("%s", I18n::tr("The run time counter did not move over the last window, so the shares cannot be worked out. The timer behind it is probably not running."));

			ImGui::Separator();
		}
	}

	/*
	 * What the CPU column's numbers are a share of.
	 *
	 * They are not a share of the wall clock: the whole is what the tasks
	 * themselves used, and the time the kernel attributes to no task - the
	 * interrupts, mostly - is not in it. A reader who took the column for a
	 * percentage of real time would be wrong by however much time went there.
	 */
	static void drawCpuColumnTooltip(const RtosSnapshot& snapshot)
	{
		if (!snapshot.cpuLoad.counting)
			return;

		ImGui::BeginTooltip();
		ImGui::Text(I18n::tr("%d of the %d tasks the window knows about could be measured, over the last %.2f s."),
		            static_cast<int>(snapshot.cpuLoad.tasksMeasured), static_cast<int>(snapshot.cpuLoad.tasksKnown), snapshot.cpuLoad.windowSeconds);
		if (snapshot.cpuLoad.tasksEstimated > 0)
			ImGui::TextDisabled("%s", I18n::tr("%d of them were counted into the whole at the rate they were last seen to use."));

		ImGui::TextDisabled("%s", I18n::tr("The shares are of the time the tasks used. Time the kernel attributes to no task - the interrupts mostly - was never in it."));
		ImGui::EndTooltip();
	}

	/*
	 * What this column is made of, for a reader who wants to know how fresh the
	 * numbers are. A tooltip on the header rather than a line of its own: the
	 * answer is wanted once, when someone wonders whether to trust the column,
	 * and not on every frame.
	 */
	static void drawStackColumnTooltip(const RtosSnapshot& snapshot)
	{
		if (!snapshot.stackScan.enabled || !snapshot.stackScan.fillSeen)
			return;

		ImGui::BeginTooltip();
		ImGui::Text(I18n::tr("Scanned on this side over %d task(s); the oldest reading is %.1f s old."), static_cast<int>(snapshot.stackScan.tasksScanned),
		            snapshot.stackScan.oldestReadingAgeSeconds);
		ImGui::TextDisabled("%s", I18n::tr("A few tasks are scanned per round, so the readings are not all from the same moment. A reading marked with a leading <= stopped at the scan limit, so the used part is at most what it says."));
		ImGui::EndTooltip();
	}


   private:
	/* The filters live as long as the program, because they are the user's
	   choice rather than the round's data. */
	inline static RtosTaskTableQuery query{};
	inline static bool stackAlertSeeded = false;

	/* Whether the detail block under the table is open. Kept with the rest of
	   what the reader set up: a fold that reopens on every start is a fold that
	   has to be folded again on every start. */
	inline static bool selectedTaskOpen = true;

	/* The event list: whether it is following the end, what it is showing, and
	   how much arrived while it was not. */
	inline static bool eventsPaused = false;
	inline static uint32_t eventsHeld = 0;

	/* How many runs the list holds: enough to scroll through, few enough that
	   a thousand switches a second do not become the whole view. */
	static constexpr size_t eventsLimit = 600;

	inline static size_t eventsCachedTotal = 0;
	inline static std::vector<RtosEventQuery::Row> eventsCache;

	/* Every match behind the rows on screen, and the task the list is limited
	   to. Zero is every task: no run has that control block. */
	inline static size_t eventsMatched = 0;
	inline static uint32_t eventsTask = 0;
	inline static uint32_t eventsCachedTask = 0;

	/* The last thing that went wrong with a recording, in the reader's words.
	   Kept until the next attempt rather than shown for a frame: a dialog that
	   failed has to say why after it has closed. */
	inline static std::string recordingProblem;
	inline static std::string playbackProblem;

	/* What the last CSV export did, and whether it went wrong. Kept beside the
	   recording problems and for the same reason: the export closes a dialog
	   and the answer has to still be there when it has. */
	inline static std::string exportNote;
	inline static bool exportFailed = false;

	/*
	 * The findings of the round on screen, worked out once and kept.
	 *
	 * Three readers want them and they are all in this class: the table's
	 * anomaly mode keeps the tasks they name, the findings block draws the
	 * chains, and the selected task says which of them it is caught in. Worked
	 * out per reader they would be worked out three times a frame, and the
	 * three answers would be from three slightly different moments.
	 *
	 * Keyed on the snapshot itself rather than on its sequence number: a
	 * sequence belongs to a session and a new session starts one again, so a
	 * number alone could match a round from an hour ago.
	 */
	inline static std::shared_ptr<const RtosSnapshot> findingsRound;
	inline static std::vector<RtosDiagnostics::Finding> findingsOfRound;

	static const std::vector<RtosDiagnostics::Finding>& findingsFor(const RtosModel& model, const std::shared_ptr<const RtosSnapshot>& snapshot)
	{
		if (findingsRound.get() != snapshot.get())
		{
			findingsOfRound = RtosDiagnostics::diagnose(*snapshot, model.history());
			findingsRound = snapshot;
		}

		return findingsOfRound;
	}

	/* The tasks the findings named, which is what the anomaly mode keeps beside
	   the two conditions a single row can answer for itself. */
	static std::vector<uint32_t> addressesNamedBy(const std::vector<RtosDiagnostics::Finding>& findings)
	{
		std::vector<uint32_t> addresses;

		for (const RtosDiagnostics::Finding& finding : findings)
		{
			for (const uint32_t address : finding.tasks)
			{
				if (address != 0)
					addresses.push_back(address);
			}
		}

		return addresses;
	}

	/* The mode as the settings file holds it. A number this build does not know
	   reads as "all tasks" rather than as whichever question happens to share
	   its number. */
	static RtosTaskFilterMode filterModeFrom(uint32_t stored)
	{
		switch (stored)
		{
			case static_cast<uint32_t>(RtosTaskFilterMode::Anomalies):
				return RtosTaskFilterMode::Anomalies;

			case static_cast<uint32_t>(RtosTaskFilterMode::Blocked):
				return RtosTaskFilterMode::Blocked;

			case static_cast<uint32_t>(RtosTaskFilterMode::HighShare):
				return RtosTaskFilterMode::HighShare;

			case static_cast<uint32_t>(RtosTaskFilterMode::All):
			default:
				return RtosTaskFilterMode::All;
		}
	}

	/* Whether the address column has been put away in this run. See the note
	   where it is hidden. */
	inline static bool addressColumnSeeded = false;

	/* What the reader is pointing at - the row they clicked, the row they are
	   on, the object the objects table should show. It lives outside this
	   class because the two halves of the view are drawn in two windows, and
	   the timeline has to point at the row this table selected. */
	static GuiRtosSelection& selection()
	{
		return GuiRtosSelection::shared();
	}

	/*
	 * The search field's buffer, kept between frames.
	 *
	 * It is not refilled from `query.nameSearch` every frame, and that is the
	 * point of it being here at all. `InputText` keeps the cursor position,
	 * the selection and any input method state against the buffer it was
	 * given; overwriting that buffer underneath it every frame leaves those
	 * out of step with the text, and the field can end up working through its
	 * own redraw over and over. On a machine using an input method - which is
	 * to say any Chinese reader's - that is not a rare path.
	 */
	inline static char searchBuffer[64] = {};
	inline static bool searchBufferPrimed = false;

	/*
	 * How bad a finding is.
	 *
	 * The four kinds are not equally urgent. Two of them describe a run that
	 * will not make progress at all, one describes a task the scheduler keeps
	 * passing over, and one describes the kernel doing exactly what it should
	 * for as long as a lock is held. Drawing all four in one colour and at one
	 * weight was the review's complaint: a section where everything is a
	 * warning is a section a reader learns to skip.
	 *
	 * The mark is a character rather than a colour so that the severity still
	 * reads for someone who cannot separate the hues.
	 */
	struct FindingSeverity
	{
		const char* mark;
		ImVec4 colour;
	};

	/* Lower is more urgent, and it is the order the two visible rows appear in. */
	static int severityRank(RtosDiagnostics::Kind kind)
	{
		switch (kind)
		{
			case RtosDiagnostics::Kind::SuspectedDeadlock:
				return 0;

			case RtosDiagnostics::Kind::UnboundedWait:
				return 1;

			case RtosDiagnostics::Kind::ReadyStarvation:
				return 2;

			case RtosDiagnostics::Kind::PriorityInheritance:
			default:
				return 3;
		}
	}

	static FindingSeverity severityOf(RtosDiagnostics::Kind kind)
	{
		/* Two palettes, like the rest of the interface. These are read as text
		   on both panels, so the light column goes darker rather than lighter:
		   the same red that reads as urgent on black turns into pink on white
		   and stops looking like a warning at all. */
		switch (kind)
		{
			case RtosDiagnostics::Kind::SuspectedDeadlock:
			case RtosDiagnostics::Kind::UnboundedWait:
				return {"!", GuiHelper::themed(ImVec4(0.90f, 0.40f, 0.35f, 1.0f), ImVec4(0.70f, 0.14f, 0.11f, 1.0f))};

			case RtosDiagnostics::Kind::ReadyStarvation:
				return {"!", GuiHelper::themed(ImVec4(0.85f, 0.58f, 0.20f, 1.0f), ImVec4(0.58f, 0.34f, 0.02f, 1.0f))};

			case RtosDiagnostics::Kind::PriorityInheritance:
			default:
				return {"i", GuiHelper::themed(ImVec4(0.55f, 0.65f, 0.85f, 1.0f), ImVec4(0.22f, 0.33f, 0.55f, 1.0f))};
		}
	}

	/* The finding's name as the interface writes it. One function rather than a
	   ternary chain at each of the two places a finding is named: the findings
	   block and the selected task's row would otherwise drift apart in wording
	   while claiming to report the same thing. */
	static const char* findingLabel(RtosDiagnostics::Kind kind)
	{
		switch (kind)
		{
			case RtosDiagnostics::Kind::SuspectedDeadlock:
				return I18n::tr("suspected deadlock");

			case RtosDiagnostics::Kind::ReadyStarvation:
				return I18n::tr("ready, never switched in");

			case RtosDiagnostics::Kind::UnboundedWait:
				return I18n::tr("waiting with no timeout");

			case RtosDiagnostics::Kind::PriorityInheritance:
			default:
				return I18n::tr("priority inheritance");
		}
	}

	/* The chain as one line, with the arrows the row draws between the nodes.
	 *
	 * What gets pasted into a message is the shape of the problem, and the shape
	 * is the chain: `inv_high -> mtx_inv -> inv_low` says what "priority
	 * inheritance" means here without any of the surrounding table. */
	static std::string chainOf(const RtosDiagnostics::Finding& finding)
	{
		std::string text;

		for (const std::string& node : finding.chain)
			text += (text.empty() ? "" : " -> ") + node;

		return text;
	}

	/*
	 * What clicking a finding selects: the first task on its chain, and the
	 * object the chain passes through.
	 *
	 * One click has to land on the same place in three views - the row in the
	 * table, the lane in the strip, the row in the objects table - and the
	 * chain is the only description of that place which all three understand.
	 */
	static void selectChain(const RtosDiagnostics::Finding& finding)
	{
		/* The filters go first. A finding names a task, and the task it names
		   may be one the current filter has hidden - locating it in the table
		   would then land on nothing, which reads as a click that did not
		   work. Clicking a finding means "take me to this", so what is hiding
		   it is dropped. */
		clearFilters();

		selection().task = 0;
		selection().object.clear();

		for (size_t node = 0; node < finding.chain.size(); node++)
		{
			const bool isTask = finding.tasks.size() > node && finding.tasks[node] != 0;

			if (isTask && selection().task == 0)
				selection().task = finding.tasks[node];
			else if (!isTask && selection().object.empty())
				selection().object = finding.chain[node];
		}
	}

	/*
	 * The findings, drawn as the chains they are.
	 *
	 * A finding is a shape - one task waiting for an object another task holds -
	 * and words alone make the reader rebuild that shape in their head. The
	 * boxes are the shape; a task box is a button, so clicking the task the
	 * finding is about selects its row, and the strip and the objects table
	 * follow.
	 *
	 * The section has a name and a count, and each row is a handle for the
	 * whole chain: a reader who has just read "waiting with no timeout" wants
	 * the task behind it, and aiming at that task's own box inside the chain
	 * is aiming twice.
	 */
	static void drawDiagnostics(const std::vector<RtosDiagnostics::Finding>& findings)
	{
		const float scale = GuiHelper::contentScale;

		/*
		 * A fixed two rows, whatever arrives.
		 *
		 * The set of findings changes as the target moves - inheritance comes
		 * and goes with a lock being held - and a block that grows by a line
		 * pushes the filters and the table below it down, then lets them back
		 * up. The reader is watching a running system; movement in the chrome
		 * is movement they did not ask for and cannot stop.
		 *
		 * Two rows rather than three: the two that mean "this run is stuck"
		 * are the ones worth the space, and a third was never seen on a target
		 * that has findings worth showing.
		 */
		constexpr size_t visibleRows = 2;

		ImGui::TextUnformatted(I18n::tr("Findings"));
		ImGui::SameLine(0.0f, 6.0f * scale);
		ImGui::TextDisabled("%d", static_cast<int>(findings.size()));

		/* What is not on screen is not dropped: the count says how many there
		   are, and this says which ones these are. */
		if (findings.size() > visibleRows)
		{
			ImGui::SameLine(0.0f, 6.0f * scale);
			GuiHelper::drawSecondaryText("%s", I18n::tr("the most serious"));
		}

		/* Most serious first, so that the two visible rows are the two worth
		   reading when there are more. */
		std::vector<const RtosDiagnostics::Finding*> ordered;
		ordered.reserve(findings.size());

		for (const RtosDiagnostics::Finding& finding : findings)
			ordered.push_back(&finding);

		std::stable_sort(ordered.begin(), ordered.end(), [](const RtosDiagnostics::Finding* left, const RtosDiagnostics::Finding* right)
		{
			return severityRank(left->kind) < severityRank(right->kind);
		});

		const float rowsTop = ImGui::GetCursorPosY();
		const float rowHeight = ImGui::GetTextLineHeightWithSpacing();

		if (ordered.empty())
			GuiHelper::drawSecondaryText("%s", I18n::tr("nothing in this round"));

		for (size_t index = 0; index < ordered.size() && index < visibleRows; index++)
		{
			const RtosDiagnostics::Finding& finding = *ordered[index];

			ImGui::PushID(&finding);

			const bool deadlock = finding.kind == RtosDiagnostics::Kind::SuspectedDeadlock;
			const bool starvation = finding.kind == RtosDiagnostics::Kind::ReadyStarvation;
			const bool unbounded = finding.kind == RtosDiagnostics::Kind::UnboundedWait;

			const char* label = findingLabel(finding.kind);

			const FindingSeverity severity = severityOf(finding.kind);

			/* The row is reserved first and filled afterwards: an invisible
			   button takes the clicks, and everything drawn over it -
			   including the small buttons inside the chain - is submitted
			   after it, which is what puts those buttons in front of it for
			   the mouse.

			   A `Selectable` was tried first and took no clicks here. The
			   button is the plainer thing and does what it says. */
			const ImVec2 rowStart = ImGui::GetCursorScreenPos();
			const float rowWidth = ImGui::GetContentRegionAvail().x;

			ImGui::InvisibleButton("##findingRow", ImVec2(rowWidth, rowHeight));

			/* The chain on the clipboard, for a bug report or a message. The
			   row already reads as one thing; the menu is the second thing it
			   can do.
			   
			   Opened by rectangle rather than through the row's item: an
			   invisible button handles the left button only, and by the time a
			   right click lands the row has already opened its tooltip - which
			   is a window of its own and takes the hover with it. Asking where
			   the mouse is sidesteps both. */
			if (ImGui::IsMouseHoveringRect(rowStart, ImVec2(rowStart.x + rowWidth, rowStart.y + rowHeight)) &&
			    ImGui::IsMouseClicked(ImGuiMouseButton_Right))
				ImGui::OpenPopup("##findingMenu");

			if (ImGui::BeginPopup("##findingMenu"))
			{
				if (ImGui::MenuItem(I18n::tr("Copy the chain")))
					ImGui::SetClipboardText(chainOf(finding).c_str());

				ImGui::EndPopup();
			}

			const bool rowHovered = ImGui::IsItemHovered();

			if (ImGui::IsItemClicked())
				selectChain(finding);

			/* The highlight is what says the row is one thing and not five:
			   the chain inside it has its own buttons, and without this the
			   row looks like loose text with a couple of boxes in it. */
			if (rowHovered)
				ImGui::GetWindowDrawList()->AddRectFilled(rowStart, ImVec2(rowStart.x + rowWidth, rowStart.y + rowHeight),
				                                          ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.22f));

			ImGui::SetCursorScreenPos(rowStart);

			/* Two encodings of the same thing: the mark says how urgent it is
			   without relying on the hue, and the colour repeats it. */
			ImGui::TextColored(severity.colour, "%s", severity.mark);
			ImGui::SameLine(0.0f, 4.0f * scale);
			ImGui::TextColored(severity.colour, "%s", I18n::tr(label));

			for (size_t node = 0; node < finding.chain.size(); node++)
			{
				const bool isTask = finding.tasks.size() > node && finding.tasks[node] != 0;

				ImGui::SameLine(0.0f, 4.0f * scale);

				/* One arrow between each pair: the chain alternates between a
				   task and the object it waits for, and words on each node
				   would have to change with the position to stay true. */
				if (node > 0)
				{
					ImGui::TextDisabled("%s", "->");
					ImGui::SameLine(0.0f, 4.0f * scale);
				}

				if (!isTask)
				{
					ImGui::TextDisabled("%s", finding.chain[node].c_str());

					continue;
				}

				const bool selected = finding.tasks[node] == selection().task;

				if (selected)
					ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

				if (ImGui::SmallButton(finding.chain[node].c_str()))
					selection().task = selected ? 0u : finding.tasks[node];

				if (selected)
					ImGui::PopStyleColor();
			}

			/* How long it has been going, at the far end of the row: the
			   numbers are what tells a moment from a hang, and a column of
			   them is easier to compare than a clause after every name. */
			if (starvation || (unbounded && finding.heldRounds > 0))
			{
				char duration[64] = {};
				std::snprintf(duration, sizeof(duration), I18n::tr("%.1f s / %u rounds"), finding.heldSeconds, finding.heldRounds);

				const float width = ImGui::CalcTextSize(duration).x;

				ImGui::SameLine();

				const float remaining = ImGui::GetContentRegionAvail().x;

				if (remaining > width + 8.0f * scale)
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + remaining - width);

				ImGui::TextDisabled("%s", duration);
			}

			if (rowHovered)
			{
				ImGui::BeginTooltip();

				if (starvation)
					ImGui::TextUnformatted(I18n::tr("A task higher in priority than the one that ran, ready in every round of the last couple of seconds. A preemptive scheduler cannot do that, except while the running task holds a lock someone above it wants - and that case is not reported here."));
				else if (unbounded)
					ImGui::TextUnformatted(I18n::tr("Blocked with no timeout: the kernel keeps the state of a task that waits forever on its suspended list, because there is no time to wake it at. It runs again only when the object is given, and if that never happens it waits for as long as the target is powered."));
				else
					ImGui::TextUnformatted(I18n::tr(deadlock ? "Two or more tasks are each waiting for a lock the next one holds. A round is a sample: if the ring clears between two of them it leaves no trace, which is why this is suspected rather than proven."
					                                         : "The task on the right is running above the priority it was created with, which the kernel only does while a higher priority task waits for a lock it holds. The lock is how long it has been held."));

				ImGui::EndTooltip();
			}

			ImGui::PopID();
		}

		/* Pad to the two rows, so that an empty or single row leaves the same
		   gap every round instead of a different one. */
		const float used = ImGui::GetCursorPosY() - rowsTop;

		if (used < rowHeight * static_cast<float>(visibleRows))
			ImGui::Dummy(ImVec2(0.0f, rowHeight * static_cast<float>(visibleRows) - used));

		ImGui::Separator();
	}

	/* Puts the next widget on this line only when it fits there.
	 *
	 * A bar of widgets joined by `SameLine` runs off the right edge of a narrow
	 * window and the last of them becomes unreachable; one that always wraps
	 * uses two lines on a wide one. Asking how much room is left is the only
	 * thing that is right at both.
	 */
	static void sameLineIfFits(float width)
	{
		if (ImGui::GetContentRegionAvail().x > width)
			ImGui::SameLine();
	}

	/*
	 * Where the rounds on screen came from.
	 *
	 * Two sources, and they are never on at once: a probe being read right now,
	 * or a file written during an earlier run. The line is one row either way,
	 * because everything under it must stay where it is when a recording starts
	 * or a file is opened - and because "is this happening now" is the first
	 * thing a reader needs to know about a table of a running system.
	 */
	static void drawSourceLine(const std::shared_ptr<RtosModel>& model, IFileHandler* fileHandler)
	{
		const float scale = GuiHelper::contentScale;

		if (model->isPlayback())
		{
			const RtosModel::PlaybackInfo info = model->playbackInfo();

			ImGui::TextUnformatted(I18n::tr("playing back"));

			ImGui::SameLine(0.0f, 6.0f * scale);
			GuiHelper::drawSecondaryText("%s", fileNameOf(info.path).c_str());

			ImGui::SameLine(0.0f, 10.0f * scale);
			GuiHelper::drawSecondaryText(I18n::tr("%u rounds"), static_cast<uint32_t>(info.rounds));

			ImGui::SameLine(0.0f, 6.0f * scale);
			GuiHelper::drawSecondaryText(I18n::tr("%.1f s"), info.lastSecond - info.firstSecond);

			/* Which round is on screen: the table is one moment out of the
			   recording, and a reader who pinned the strip needs to see which
			   moment that turned out to be. */
			if (info.rounds > 0)
			{
				ImGui::SameLine(0.0f, 6.0f * scale);
				GuiHelper::drawSecondaryText(I18n::tr("round %u of %u"), static_cast<uint32_t>(info.current + 1),
				                             static_cast<uint32_t>(info.rounds));
			}

			sameLineIfFits(120.0f * scale);

			if (ImGui::SmallButton(I18n::tr("back to live")))
			{
				exportNote.clear();
				model->closePlayback();
			}

			/* Another file is one click away, rather than a trip through the
			   live view to get back to the same button. */
			sameLineIfFits(150.0f * scale);

			if (ImGui::SmallButton(I18n::tr("open a recording")))
				openRecording(model, fileHandler);

			if (!playbackProblem.empty())
			{
				ImGui::SameLine(0.0f, 8.0f * scale);
				ImGui::TextColored(GuiHelper::redLight(), "%s", playbackProblem.c_str());
			}

			/*
			 * On its own line, and the line above is full at every width this
			 * runs at: a button that fits only on a wide screen is a button
			 * that is missing on the machine somebody actually uses. The row
			 * above says what is being looked at; this one says what can be
			 * taken away, and the answer to what it wrote lands beside it.
			 */
			if (ImGui::SmallButton(I18n::tr("export CSV")))
				exportPlayback(model, fileHandler);

			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
				ImGui::TextUnformatted(
				    I18n::tr("Writes every round to a spreadsheet: one line per task per round, with the columns the table shows. A field the round did not measure is left empty rather than written as zero."));
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}

			if (!exportNote.empty())
			{
				ImGui::SameLine(0.0f, 8.0f * scale);

				if (exportFailed)
					ImGui::TextColored(GuiHelper::redLight(), "%s", exportNote.c_str());
				else
					GuiHelper::drawSecondaryText("%s", exportNote.c_str());
			}

			ImGui::Separator();

			return;
		}

		if (model->isRecording())
		{
			ImGui::TextColored(GuiHelper::redLight(), "%s", I18n::tr("recording"));

			const RtosRecording::Info info = model->recordingStats();

			ImGui::SameLine(0.0f, 6.0f * scale);
			GuiHelper::drawSecondaryText("%s", fileNameOf(model->recordingPath()).c_str());

			ImGui::SameLine(0.0f, 10.0f * scale);
			GuiHelper::drawSecondaryText(I18n::tr("%u rounds"), static_cast<uint32_t>(info.rounds));

			ImGui::SameLine(0.0f, 6.0f * scale);
			GuiHelper::drawSecondaryText(I18n::tr("%.1f s"), info.durationSeconds());

			/* The size is part of the answer, not decoration: a recording of a
			   hundred rounds a second is six megabytes a minute, and a reader
			   who leaves it on for an afternoon should be told. */
			ImGui::SameLine(0.0f, 6.0f * scale);

			if (info.bytes < 1024u * 1024u)
				GuiHelper::drawSecondaryText(I18n::tr("%.0f KB"), static_cast<double>(info.bytes) / 1024.0);
			else
				GuiHelper::drawSecondaryText(I18n::tr("%.1f MB"), static_cast<double>(info.bytes) / (1024.0 * 1024.0));

			sameLineIfFits(120.0f * scale);

			if (ImGui::SmallButton(I18n::tr("stop recording")))
				model->stopRecording();
		}
		else
		{
			if (ImGui::SmallButton(I18n::tr("record")))
				startRecording(model, fileHandler);

			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
				ImGui::TextUnformatted(I18n::tr("Writes every round the view is shown to a file, so that the same table, findings and lanes can be read again with no probe attached."));
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}

			sameLineIfFits(150.0f * scale);

			if (ImGui::SmallButton(I18n::tr("open a recording")))
				openRecording(model, fileHandler);
		}

		if (!recordingProblem.empty())
		{
			ImGui::SameLine(0.0f, 8.0f * scale);
			ImGui::TextColored(GuiHelper::redLight(), "%s", recordingProblem.c_str());
		}

		ImGui::Separator();
	}

	static std::string fileNameOf(const std::string& path)
	{
		if (path.empty())
			return {};

		return std::filesystem::path(path).filename().string();
	}

	/* A name that sorts by when it was taken, so that a directory of them reads
	   as a timeline rather than as a pile. */
	static std::string defaultRecordingName()
	{
		const std::time_t now = std::time(nullptr);
		std::tm local = {};
		localtime_s(&local, &now);

		char stamp[32] = {};
		std::strftime(stamp, sizeof(stamp), "tasks_%Y%m%d-%H%M%S.vtrec", &local);

		return stamp;
	}

	static void startRecording(const std::shared_ptr<RtosModel>& model, IFileHandler* fileHandler)
	{
		recordingProblem.clear();

		if (fileHandler == nullptr)
			return;

		std::string path = fileHandler->saveFile({{I18n::tr("Task view recordings"), "vtrec"}, {I18n::tr("All files"), "*"}}, "",
		                                         defaultRecordingName());

		if (path.empty())
			return;

		if (path.size() < 6 || path.compare(path.size() - 6, 6, ".vtrec") != 0)
			path += ".vtrec";

		RtosRecording::Problem problem = RtosRecording::Problem::None;

		if (!model->startRecording(path, problem))
			recordingProblem = I18n::tr(RtosRecording::describe(problem));
	}

	static void openRecording(const std::shared_ptr<RtosModel>& model, IFileHandler* fileHandler)
	{
		playbackProblem.clear();
		exportNote.clear();

		if (fileHandler == nullptr)
			return;

		const std::string path = fileHandler->openFile({{I18n::tr("Task view recordings"), "vtrec"}, {I18n::tr("All files"), "*"}});

		if (path.empty())
			return;

		RtosRecording::Problem problem = RtosRecording::Problem::None;

		if (!model->openRecording(path, problem))
			playbackProblem = I18n::tr(RtosRecording::describe(problem));
	}

	/*
	 * The recording, written out for something else to read.
	 *
	 * Done on this thread, in one pass, rather than handed to a worker: the
	 * whole file is a read and a write per round, and a recording of ten
	 * thousand rounds takes about a second. A progress bar for a second of
	 * work would be its own complication, and the rounds are read through the
	 * same decoder the view uses, so what lands in the file is what was on
	 * screen.
	 */
	static void exportPlayback(const std::shared_ptr<RtosModel>& model, IFileHandler* fileHandler)
	{
		exportNote.clear();
		exportFailed = false;

		if (model == nullptr || fileHandler == nullptr || !model->isPlayback())
			return;

		const RtosModel::PlaybackInfo info = model->playbackInfo();

		if (info.rounds == 0)
		{
			exportFailed = true;
			exportNote = I18n::tr("This recording holds no rounds.");

			return;
		}

		/* The recording's own name with a CSV extension, because the two files
		   are the same session and a reader who found one should find the other
		   beside it. */
		std::string name = fileNameOf(info.path);

		const size_t dot = name.find_last_of('.');

		if (dot != std::string::npos)
			name = name.substr(0, dot);

		name += ".csv";

		std::string path = fileHandler->saveFile({{I18n::tr("CSV files"), "csv"}, {I18n::tr("All files"), "*"}}, "", name);

		if (path.empty())
			return;

		if (path.size() < 4 || path.compare(path.size() - 4, 4, ".csv") != 0)
			path += ".csv";

		const std::filesystem::path target(path);
		std::error_code error;

		if (target.has_parent_path())
			std::filesystem::create_directories(target.parent_path(), error);

		std::ofstream file(path, std::ios::out | std::ios::trunc);

		if (!file)
		{
			exportFailed = true;
			exportNote = I18n::tr("The file could not be created.");

			return;
		}

		const RtosCsvExport::Counts counts = RtosCsvExport::writeRecording(file, *model);

		file.close();

		char text[512] = {};

		if (counts.skipped > 0)
		{
			exportFailed = true;

			std::snprintf(text, sizeof(text), I18n::tr("Wrote %u rows; %u rounds could not be read and were left out."),
			              static_cast<uint32_t>(counts.rows), static_cast<uint32_t>(counts.skipped));
		}
		else
		{
			std::snprintf(text, sizeof(text), I18n::tr("Wrote %u rows from %u rounds to %s"), static_cast<uint32_t>(counts.rows),
			              static_cast<uint32_t>(counts.rounds), fileNameOf(path).c_str());
		}

		exportNote = text;
	}

	/*
	 * What the round says, before the table says it row by row.
	 *
	 * The counts are also the filters: a reader who sees seven blocked tasks and
	 * wants to know which ones should not have to type into a search box first.
	 */
	static void drawSummary(const RtosSnapshot& snapshot, const std::vector<RtosDiagnostics::Finding>& findings)
	{
		const float scale = GuiHelper::contentScale;

		const RtosTableModel::Summary summary = RtosTableModel::summaryOf(snapshot, query.stackAlertFreePercent);

		ImGui::Text(I18n::tr("%u tasks"), summary.tasks);

		sameLineIfFits(120.0f * scale);
		stateChip(summary.running, RtosTaskState::Running);

		sameLineIfFits(120.0f * scale);
		stateChip(summary.ready, RtosTaskState::Ready);

		sameLineIfFits(120.0f * scale);
		stateChip(summary.blocked, RtosTaskState::Blocked);

		sameLineIfFits(120.0f * scale);
		stateChip(summary.suspended, RtosTaskState::Suspended);

		/* The two findings worth acting on, and only when there are any: a
		   summary that lists zeroes makes the reader read it every round. */
		if (summary.stackAlerts > 0)
		{
			sameLineIfFits(170.0f * scale);

			ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::orangeLight());

			const std::string label = std::to_string(summary.stackAlerts) + " " + I18n::tr("stack alerts");

			if (ImGui::SmallButton(label.c_str()))
				query.onlyStackAlerts = !query.onlyStackAlerts;

			ImGui::PopStyleColor();

			if (query.onlyStackAlerts)
			{
				ImGui::SameLine(0.0f, 4.0f * scale);
				ImGui::TextDisabled("%s", I18n::tr("filtered"));
			}
		}

		if (summary.inherited > 0)
		{
			sameLineIfFits(170.0f * scale);

			GuiHelper::drawSecondaryText("%u %s", summary.inherited, I18n::tr("priority inherited"));

			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(I18n::tr("A task whose current priority is above its base one is holding a lock that a higher priority task is waiting for."));
				ImGui::EndTooltip();
			}
		}

		/*
		 * The three questions the lanes cannot answer without being counted.
		 *
		 * A strip of colour says which task was in which state; it does not say
		 * that one task used 38 % of the time or that another has been waiting
		 * two minutes. Those are the answers a reader comes for, and they are
		 * one subtraction away from data already in hand.
		 */
		const RtosTask* busiest = nullptr;

		for (const RtosTask& task : snapshot.tasks)
		{
			if (!task.hasCpuShare)
				continue;

			if (busiest == nullptr || task.cpuSharePercent > busiest->cpuSharePercent)
				busiest = &task;
		}

		if (busiest != nullptr && busiest->cpuSharePercent > 0.0)
		{
			sameLineIfFits(190.0f * scale);
			GuiHelper::drawSecondaryText(I18n::tr("busiest %s %.1f%%"), busiest->name.empty() ? unknown() : busiest->name.c_str(),
			                             busiest->cpuSharePercent);
		}

		/* The longest wait comes from the findings rather than from a column:
		   a task that has been blocked for two minutes is already named there,
		   with the object it waits for. */
		const RtosDiagnostics::Finding* longestWait = nullptr;

		for (const RtosDiagnostics::Finding& finding : findings)
		{
			if (finding.kind != RtosDiagnostics::Kind::UnboundedWait)
				continue;

			if (longestWait == nullptr || finding.heldSeconds > longestWait->heldSeconds)
				longestWait = &finding;
		}

		/* The second row, deliberately: the first one is full by the time the
		   busiest task is named, and a width estimate that lets this through
		   anyway cuts the answer in half - "longes" is not an answer. */
		if (longestWait != nullptr && !longestWait->chain.empty())
		{
			GuiHelper::drawSecondaryText(I18n::tr("longest wait %s %.0f s"), longestWait->chain.front().c_str(), longestWait->heldSeconds);

			if (summary.unread > 0)
			{
				ImGui::SameLine(0.0f, 12.0f * scale);
				GuiHelper::drawSecondaryText("%s %u", I18n::tr("not read"), summary.unread);
			}
		}
		else if (summary.unread > 0)
			GuiHelper::drawSecondaryText("%s %u", I18n::tr("not read"), summary.unread);

		ImGui::Separator();
	}

	/* A count that is also a filter: clicking it narrows the table to those rows,
	   clicking it again lets them all back. */
	/*
	 * A count that is also a filter.
	 *
	 * The colour here used to do three jobs with one blue fill: it said which
	 * state the count was about, it said whether the filter was on, and it
	 * looked like every other button in the row. A reader could not tell a
	 * state's colour from a filter that had been applied.
	 *
	 * The work is split now. The state gets a dot of its own - the same hue
	 * the timeline's legend uses, so a count and a lane are the same colour -
	 * and the fill is reserved for one meaning: this filter is on. Off is no
	 * fill at all, just a dot, a number and a word; on is a wash of the
	 * state's colour with a border around it.
	 */
	static void stateChip(uint32_t count, RtosTaskState state)
	{
		const float scale = GuiHelper::contentScale;
		const bool active = query.states.size() == 1 && query.states.front() == state;

		const ImVec4 face = ImGui::ColorConvertU32ToFloat4(GuiRtosPanelTimeline::getStateColour(state));
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float lineHeight = ImGui::GetFrameHeight();
		const float dotSize = 7.0f * scale;

		ImGui::Dummy(ImVec2(dotSize + 4.0f * scale, lineHeight));

		/* Drawn, not printed: no bullet in the interface font. */
		ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(origin.x + dotSize * 0.5f, origin.y + lineHeight * 0.5f), dotSize * 0.5f,
		                                            ImGui::GetColorU32(face), 12);

		ImGui::SameLine(0.0f, 2.0f * scale);

		ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(face.x, face.y, face.z, 0.38f) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(face.x, face.y, face.z, 0.26f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(face.x, face.y, face.z, 0.48f));
		ImGui::PushStyleColor(ImGuiCol_Border, face);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, active ? 1.0f : 0.0f);

		const std::string label = std::to_string(count) + " " + RtosTableModel::taskStateLabel(state);

		if (ImGui::SmallButton(label.c_str()))
		{
			if (active)
				query.states.clear();
			else
				query.states = {state};

			/* The other half of the rule in `setMode`: a chip is the same
			   request as the blocked mode, so choosing one by hand ends that
			   mode rather than intersecting with it. Two controls that both
			   narrow by state and disagreed would leave a table that is empty
			   for a reason no single control shows. */
			if (query.mode == RtosTaskFilterMode::Blocked)
				query.mode = RtosTaskFilterMode::All;
		}

		ImGui::PopStyleVar();
		ImGui::PopStyleColor(4);
	}

	static void clearFilters()
	{
		query.nameSearch.clear();
		query.states.clear();
		query.hasPriorityFloor = false;
		query.priorityFloor = 0;
		query.onlyStackAlerts = false;

		/* The mode is a filter like the others, and the button says "clear
		   filters": leaving the narrowest of them on would make the button look
		   as though it had not worked. */
		query.mode = RtosTaskFilterMode::All;

		/* The search field draws from its own buffer, so clearing the query alone
		   would leave the old text in the box. */
		std::snprintf(searchBuffer, sizeof(searchBuffer), "%s", "");
	}

	/* Whether any filter is narrowing the table. Used to decide whether the
	   button that clears them is worth pressing, so that it does not sit there
	   lit up with nothing to do. */
	static bool filterIsOn()
	{
		return !query.nameSearch.empty() || !query.states.empty() || query.onlyStackAlerts || query.hasPriorityFloor ||
		       query.mode != RtosTaskFilterMode::All;
	}

	/*
	 * Which states to keep, as one menu.
	 *
	 * There used to be a checkbox for blocked alone, while the summary above
	 * had a chip for each of the four states - two ways to narrow by state, one
	 * of which could only ever name one of them. The menu covers all four and
	 * the chips remain as the shortcut, and both write the same field, so
	 * neither can disagree with the other.
	 */
	static void drawStateMenu()
	{
		const float scale = GuiHelper::contentScale;

		std::string label = I18n::tr("states");

		if (!query.states.empty())
			label += " (" + std::to_string(query.states.size()) + ")";

		if (ImGui::Button((label + "##rtosStates").c_str(), ImVec2(92.0f * scale, 0.0f)))
			ImGui::OpenPopup("##rtosStatesMenu");

		if (!ImGui::BeginPopup("##rtosStatesMenu"))
			return;

		const RtosTaskState states[] = {RtosTaskState::Running, RtosTaskState::Ready, RtosTaskState::Blocked, RtosTaskState::Suspended};

		for (const RtosTaskState state : states)
		{
			const auto found = std::find(query.states.begin(), query.states.end(), state);
			bool on = found != query.states.end();

			/* The mark the state has in the strip, beside its name: the same
			   pairing the summary uses. */
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const float lineHeight = ImGui::GetFrameHeight();
			const float dotSize = 7.0f * scale;

			ImGui::Dummy(ImVec2(dotSize + 4.0f * scale, lineHeight));
			ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(origin.x + dotSize * 0.5f, origin.y + lineHeight * 0.5f), dotSize * 0.5f,
			                                            GuiRtosPanelTimeline::getStateColour(state), 12);
			ImGui::SameLine(0.0f, 2.0f * scale);

			if (ImGui::Checkbox(RtosTableModel::taskStateLabel(state), &on))
			{
				if (on)
					query.states.push_back(state);
				else
					query.states.erase(found);
			}
		}

		ImGui::Separator();

		if (ImGui::MenuItem(I18n::tr("show every state")))
			query.states.clear();

		ImGui::EndPopup();
	}

	/*
	 * The lowest priority worth showing.
	 *
	 * Written as the comparison it is: "priority floor 2" is the name of the
	 * field in the code, and a reader looking at a filter bar has to work out
	 * whether the number is a minimum, a maximum, or a count.
	 */
	static void drawPriorityFloor()
	{
		const float scale = GuiHelper::contentScale;

		if (ImGui::Checkbox(I18n::tr("priority >="), &query.hasPriorityFloor))
		{
			/* Turning the floor on with nothing set would hide every task that
			   has a priority, which reads as a broken table rather than as a
			   filter someone asked for. */
			if (query.hasPriorityFloor && query.priorityFloor == 0)
				query.priorityFloor = 1;
		}

		if (!query.hasPriorityFloor)
			return;

		ImGui::SameLine(0.0f, 4.0f * scale);
		ImGui::SetNextItemWidth(60.0f * scale);

		int floor = static_cast<int>(query.priorityFloor);

		if (ImGui::InputInt("##rtosFloor", &floor, 0, 0))
			query.priorityFloor = floor < 0 ? 0u : static_cast<uint32_t>(floor);
	}

	/*
	 * What narrows the table, in three rows: the question, the filters that
	 * narrow inside it, and the two thresholds that are set once.
	 *
	 * Every number carries its unit and every condition reads as a sentence
	 * now. "stack alert at 20" was twenty of something the reader had to guess
	 * at, and the button that resets the sort sat among the filters although it
	 * is a table setting - the header already sorts, and clicking the sorted
	 * column turns it over, so the button is gone.
	 *
	 * Returns the rows the query keeps, so that whoever draws them next does not
	 * ask the same question a second time - and so that the count on the last
	 * row and the rows under it cannot disagree.
	 */
	/*
	 * The rows of the table: one per task the session has seen.
	 *
	 * Not one per task this round holds. A round of fourteen followed by one of
	 * eleven used to take three rows away and put them back half a second
	 * later, and the reader was left re-finding their place. What the round
	 * says about a task it did read is the row's content; what it says about
	 * one it did not is nothing, and that is drawn as a mark rather than by
	 * deleting the row.
	 */
	static std::vector<RtosTaskRow> currentRows(const RtosModel& model, const RtosSnapshot& snapshot, const std::vector<uint32_t>& named)
	{
		return RtosTableModel::taskRows(model.rosterEntries(), snapshot, query, named);
	}

	static std::vector<RtosTaskRow> drawFilters(const RtosModel& model, const std::shared_ptr<const RtosSnapshot>& snapshot,
	                                            const std::vector<uint32_t>& named)
	{
		const float scale = GuiHelper::contentScale;

		drawModes();

		if (!searchBufferPrimed)
		{
			std::snprintf(searchBuffer, sizeof(searchBuffer), "%s", query.nameSearch.c_str());
			searchBufferPrimed = true;
		}

		ImGui::SetNextItemWidth(180 * scale);

		if (ImGui::InputTextWithHint("##rtosNameSearch", I18n::tr("search by name"), searchBuffer, sizeof(searchBuffer)))
			query.nameSearch = searchBuffer;

		sameLineIfFits(120.0f * scale);
		drawStateMenu();

		sameLineIfFits(150.0f * scale);
		ImGui::Checkbox(I18n::tr("stack alerts only"), &query.onlyStackAlerts);

		/* Lit up only when there is something to clear, and flat when there is
		   not: a disabled button keeps its face colour here, so the face is
		   taken away as well, which is what makes "nothing to clear" read as
		   a fact rather than as a button that looks wrong. */
		const bool anyFilter = filterIsOn();

		sameLineIfFits(130.0f * scale);
		ImGui::BeginDisabled(!anyFilter);

		if (!anyFilter)
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

		if (ImGui::Button(I18n::tr("clear filters")))
			clearFilters();

		if (!anyFilter)
			ImGui::PopStyleColor();

		ImGui::EndDisabled();

		/* Row two: the thresholds. No `SameLine` before the first of them, so
		   they start a line of their own. */
		drawAlertThreshold();

		sameLineIfFits(170.0f * scale);
		drawPriorityFloor();

		/* The count goes to the far end of the row, away from the threshold
		   beside it: written close after "priority >=" it read as part of the
		   condition. */
		const std::vector<RtosTaskRow> rows = currentRows(model, *snapshot, named);

		char shown[64] = {};
		std::snprintf(shown, sizeof(shown), I18n::tr("%d of %d shown"), static_cast<int>(rows.size()), static_cast<int>(model.rosterSize()));

		const float shownWidth = ImGui::CalcTextSize(shown).x;

		ImGui::SameLine();

		const float remaining = ImGui::GetContentRegionAvail().x;

		if (remaining > shownWidth + 16.0f * scale)
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + remaining - shownWidth);

		GuiHelper::drawSecondaryText("%s", shown);

		ImGui::Separator();

		return rows;
	}

	/*
	 * The four questions, as four buttons on one line.
	 *
	 * Four buttons rather than a bar that hides the ones with nothing in them:
	 * the count behind each question changes with the target, and a row that
	 * grew and shrank its buttons would move the one the reader is reaching for.
	 *
	 * The fill means what it means on the state chips below - this is the
	 * question being asked - so the two bars cannot be read as two different
	 * kinds of selection.
	 */
	static void drawModes()
	{
		const float scale = GuiHelper::contentScale;

		modeButton(I18n::tr("all tasks"), RtosTaskFilterMode::All, I18n::tr("Every task the round read, whatever state it is in."));

		ImGui::SameLine(0.0f, 4.0f * scale);
		modeButton(I18n::tr("problems only"), RtosTaskFilterMode::Anomalies,
		           I18n::tr("A stack at or below the alert threshold, a priority raised by inheritance, and every task the findings above name - a deadlock ring, a wait with no timeout, a task ready and passed over. A task waiting on an object with a timeout is not one of these: in a healthy kernel that is most of them."));

		ImGui::SameLine(0.0f, 4.0f * scale);
		modeButton(I18n::tr("blocked only"), RtosTaskFilterMode::Blocked, I18n::tr("Tasks waiting for something, whether or not the wait has a timeout."));

		ImGui::SameLine(0.0f, 4.0f * scale);
		modeButton(I18n::tr("high share only"), RtosTaskFilterMode::HighShare,
		           I18n::tr("The tasks whose share of the task time is at or above the floor. A share needs two rounds and a target built with run time statistics."));

		if (query.mode != RtosTaskFilterMode::HighShare)
			return;

		/* The floor, beside the question it belongs to and only while that
		   question is the one being asked: a number on screen every round that
		   matters once a session is a number the reader has to work out the
		   purpose of. */
		sameLineIfFits(230.0f * scale);

		ImGui::TextUnformatted(I18n::tr("task share"));
		ImGui::SameLine(0.0f, 4.0f * scale);
		ImGui::TextUnformatted(">=");
		ImGui::SameLine(0.0f, 4.0f * scale);
		ImGui::SetNextItemWidth(70.0f * scale);

		float floor = static_cast<float>(query.shareFloorPercent);

		if (ImGui::InputFloat("##rtosShareFloor", &floor, 0.0f, 0.0f, "%.0f"))
			query.shareFloorPercent = std::clamp(static_cast<double>(floor), 0.0, 100.0);

		ImGui::SameLine(0.0f, 4.0f * scale);
		ImGui::TextUnformatted("%");

		if (ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(I18n::tr("A task whose share of the task time is at or above this is kept. Zero keeps every task that has a share. The share is of the time the tasks used, not of the wall clock."));
			ImGui::EndTooltip();
		}
	}

	static void modeButton(const char* label, RtosTaskFilterMode mode, const char* tooltip)
	{
		const bool active = query.mode == mode;

		ImGui::PushStyleColor(ImGuiCol_Button, active ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_Text));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, active ? 1.0f : 0.0f);

		if (ImGui::SmallButton(label))
			setMode(mode);

		ImGui::PopStyleVar();
		ImGui::PopStyleColor(2);

		if (ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
			ImGui::TextUnformatted(tooltip);
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	/*
	 * Switching the question, and the one state the mode answers by itself.
	 *
	 * `blocked only` and the blocked chip are the same request, so the mode
	 * carries it and the chip's list is emptied as it arrives - otherwise a
	 * reader who had asked for the ready tasks and then for the blocked ones
	 * would be asking for nothing and would get exactly that, with two lit
	 * controls apparently agreeing.
	 */
	static void setMode(RtosTaskFilterMode mode)
	{
		query.mode = mode;

		if (mode == RtosTaskFilterMode::Blocked)
			query.states.clear();
	}

	/*
	 * Why the table is empty, in the words of the question that emptied it.
	 *
	 * Three different things produce an empty table and they look identical: a
	 * question with no answer in this round, a filter that kept nothing, and a
	 * target that cannot answer the question at all. The last is what a reader
	 * would take for a broken panel - a kernel built without run time
	 * statistics has no shares, so nothing can be above a share floor.
	 */
	static const char* emptyWords(const RtosSnapshot& snapshot)
	{
		switch (query.mode)
		{
			case RtosTaskFilterMode::Anomalies:
				return I18n::tr("Nothing in this round looks wrong: no stack at the alert threshold, no inherited priority, and no finding naming a task.");

			case RtosTaskFilterMode::Blocked:
				return I18n::tr("No task is blocked in this round.");

			case RtosTaskFilterMode::HighShare:
				for (const RtosTask& task : snapshot.tasks)
				{
					if (task.hasCpuShare)
						return I18n::tr("No task is at or above that share of the task time.");
				}

				return I18n::tr("The target does not count the time each task used, so no share can be above the floor.");

			case RtosTaskFilterMode::All:
			default:
				return I18n::tr("No task matches these filters.");
		}
	}

	/*
	 * The threshold the stack alert is measured against.
	 *
	 * It starts at what the settings file says and can be moved here, because
	 * the right number is a judgement about one target: a stack that is
	 * comfortable on one is nearly full on another, and the moment that
	 * judgement gets made is while looking at the table. Zero turns the mark
	 * off, and the number is not written back - it belongs to the run.
	 */
	static void drawAlertThreshold()
	{
		const float scale = GuiHelper::contentScale;

		/* A sentence with its unit: "stack free <= 20 %". The old "stack alert
		   at 20" left the reader to guess at twenty of what. */
		ImGui::TextUnformatted(I18n::tr("stack free"));
		ImGui::SameLine(0.0f, 4.0f * scale);
		ImGui::TextUnformatted("<=");
		ImGui::SameLine(0.0f, 4.0f * scale);
		ImGui::SetNextItemWidth(70.0f * scale);

		int percent = static_cast<int>(query.stackAlertFreePercent);

		if (ImGui::InputInt("##rtosStackAlert", &percent, 0, 0))
			query.stackAlertFreePercent = percent < 0 ? 0u : static_cast<uint32_t>(percent);

		ImGui::SameLine(0.0f, 4.0f * scale);
		ImGui::TextUnformatted("%");

		if (ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(I18n::tr("A task whose stack is this empty or less is marked. Zero turns the marking off, and a task with no reading is never marked."));
			ImGui::EndTooltip();
		}
	}

	/*
	 * The rows, drawn.
	 *
	 * The rows arrive rather than being asked for here: the filters decided
	 * them a few lines above, the count under them was taken from the same set,
	 * and asking again would be the one place where the two could disagree
	 * about what is on screen.
	 */
	/*
	 * A task the newest round did not read.
	 *
	 * The name and the address are identity - the session knows them - so they
	 * are shown; every column that would hold a reading gets a slash. The
	 * tooltip says how long ago the task was last read, because "not read" on
	 * its own leaves the reader wondering whether the task is gone.
	 */
	static void drawMissingRow(const RtosTaskRow& row)
	{
		const bool selected = row.tcbAddress == selection().task;

		if (ImGui::Selectable(row.name.empty() ? missingRow() : row.name.c_str(), selected,
		                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap))
			selection().task = selected ? 0u : row.tcbAddress;

		if (ImGui::IsItemHovered())
		{
			selection().hoveredTask = row.tcbAddress;

			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
			ImGui::Text(I18n::tr("This round did not read this task, so every column is a slash. The row stays: the task is there, the reading is not. Its last reading was %.1f s before this round."),
			            row.sinceLastReadSeconds);
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::TableNextColumn();
		ImGui::Text("0x%08X", row.tcbAddress);

		/* Priority, state, stack size, stack used, share, waiting object. */
		for (int column = 0; column < 6; column++)
		{
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(missingRow());
		}
	}

	static void drawTable(const std::shared_ptr<const RtosSnapshot>& snapshot, const std::vector<RtosTaskRow>& rows, float height)
	{
		/*
		 * Every column has a width of its own, and the sideways scroll is the
		 * fallback for a pane dragged narrow.
		 *
		 * A stretching column was tried first and does not work here: with
		 * `ScrollX` in this version the stretch column is measured against
		 * something other than what is left, and the last column pays for it -
		 * cut off at the border, "Waiting on" arriving as "Waiting". Widths
		 * that are stated are widths that hold, and the scrollbar appears only
		 * when the pane really is too narrow for them.
		 */
		const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
		                              ImGuiTableFlags_ScrollX | ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_Hideable;

		/*
		 * The table is given a height rather than the whole window.
		 *
		 * With `ScrollY` and no height it takes everything that is left, which
		 * pushed the synchronisation objects below it clean out of the window
		 * - they were drawn, and drawn out of sight. The share is taken of
		 * what is available at this point, so the two tables between them fill
		 * the view whatever its size.
		 */
		/*
		 * The table fills what is left of the pane, and its height has nothing
		 * to do with how many tasks there are.
		 *
		 * It used to follow the row count - six rows minimum, twenty maximum -
		 * on the reasoning that a fixed share left empty table under a short
		 * list. What that reasoning missed is that the count is not stable
		 * while the target runs: a task blocks, a filter cuts the list to two,
		 * and the table shrank and grew under the reader's eye, taking the
		 * scroll position and the sense of "where am I" with it. Empty space at
		 * the bottom is still empty space whether it is inside the table or
		 * under it; a table that changes size while you read it is worse.
		 *
		 * `ScrollY` is on, so a list longer than the pane scrolls inside it and
		 * the height still does not move.
		 */
		const ImVec2 size(0.0f, height);

		/*
		 * The name carries a version. ImGui stores a table's column widths and
		 * which columns are shown under its name in imgui.ini, so a table that
		 * is given better widths under the old name keeps the old ones - for
		 * every user who has run the program before. Changing the name is what
		 * makes a column layout take effect on an existing installation, and
		 * bumping it is what a later layout change should do.
		 */
		if (!ImGui::BeginTable("rtosTasks-v2", 8, flags, size))
			return;

		ImGui::TableSetupScrollFreeze(0, 1);

		/*
		 * Every column is registered before any row is begun.
		 *
		 * Calling `TableSetupColumn` and `TableNextColumn` in turn looks
		 * reasonable and is not: `TableNextColumn` begins the first row, and
		 * beginning a row locks the layout. The next `TableSetupColumn` then
		 * trips the assertion in `imgui_tables.cpp` that says exactly this.
		 * With assertions on, that is an `abort` - the window hangs on the
		 * dialog for a moment and then the process is gone.
		 */
		/*
		 * Widths by what the column holds, not evenly across the eight.
		 *
		 * The address is the widest fixed thing in the table and the one least
		 * needed at a glance, so it starts hidden - the right click menu brings
		 * it back. The columns between Name and Waiting object are short numbers
		 * or short words and get a width that fits them; the two names take what
		 * is left, because they are the two that were being cut.
		 *
		 * Every width carries the interface scale. The numbers below are what
		 * a column holds at a scale of one; a window on a scaled display draws
		 * its text at that scale too, and a width written plainly then holds
		 * two thirds of a word - "Priori", "Task shar" - while the column
		 * looks as if it had room to spare.
		 */
		const float columnScale = GuiHelper::contentScale;

		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::Name), ImGuiTableColumnFlags_WidthFixed, 120.0f * columnScale);
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::Address), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 95.0f * columnScale);
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::Priority), ImGuiTableColumnFlags_WidthFixed, 60.0f * columnScale);
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::State), ImGuiTableColumnFlags_WidthFixed, 66.0f * columnScale);
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::StackSize), ImGuiTableColumnFlags_WidthFixed, 45.0f * columnScale);

		/* Wide enough for the widest reading it can hold - "576 / 768
		   (75.2%)" - and not for the header alone, which is the shorter of the
		   two. */
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::StackWatermark), ImGuiTableColumnFlags_WidthFixed, 106.0f * columnScale);

		/* Wide enough for the header as well as the number: "Task share" is
		   the one name in the table that has to be read in full, because a
		   reader who takes the column for the machine's utilisation has read
		   the table wrong. */
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::CpuLoad), ImGuiTableColumnFlags_WidthFixed, 74.0f * columnScale);
		/* Wide enough for the header, and for the longest object name the
		   fixture has; the tooltip carries the rest. */
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::WaitingOn), ImGuiTableColumnFlags_WidthFixed, 90.0f * columnScale);

		/*
		 * The address starts hidden, by hand.
		 *
		 * It is the widest fixed thing in the table and the one least needed
		 * at a glance, so it was given `ImGuiTableColumnFlags_DefaultHide` -
		 * which only applies while no column settings have been saved for the
		 * table, and stops applying the moment the layout file has any. That
		 * is every run after the first, which makes the flag useless for this.
		 * Hiding it once and letting the column menu bring it back is what
		 * holds, because the choice then belongs to the table and is saved
		 * with the rest of it. Once per run: a reader who turns the column
		 * back on must not have it taken away again a frame later.
		 */
		if (!addressColumnSeeded)
		{
			ImGui::TableSetColumnEnabled(1, false);
			addressColumnSeeded = true;
		}

			drawHeaderRow(snapshot);

		selection().hoveredTask = 0;
		selection().tableDrewThisFrame = true;

		for (const RtosTaskRow& row : rows)
		{
			ImGui::TableNextRow();

			ImGui::TableNextColumn();

			if (!row.hasReading)
			{
				/* Dimmed, so that a row of slashes does not compete with the
				   readings above it for the eye. */
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.62f);
				drawMissingRow(row);
				ImGui::PopStyleVar();

				continue;
			}

			/*
			 * The name doubles as the row's handle.
			 *
			 * Clicking a row is how the three areas are joined: the timeline
			 * highlights the lane of the task picked here, and the objects
			 * table points at whatever that task is waiting on. Without it a
			 * reader comparing a row with a lane is doing it by eye, and by
			 * eye is where the wrong lane gets read.
			 */
			const bool selected = row.tcbAddress == selection().task;

			if (ImGui::Selectable(row.name.empty() ? unknown() : row.name.c_str(), selected,
			                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap))
			{
				selection().task = selected ? 0u : row.tcbAddress;

				if (!selected && !row.waitingOn.empty())
					selection().object = row.waitingOn;
			}

			if (ImGui::IsItemHovered())
				selection().hoveredTask = row.tcbAddress;

			ImGui::TableNextColumn();
			ImGui::Text("0x%08X", row.tcbAddress);

			/* Priority: the base value is shown beside the current one when
			   they differ, because that difference is the whole visible sign
			   of priority inheritance and is otherwise invisible. */
			ImGui::TableNextColumn();

			if (!row.hasPriority)
				ImGui::TextUnformatted(unknown());
			else if (row.hasBasePriority && row.basePriority != row.priority)
			{
				ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImVec4(0.55f, 0.35f, 0.10f, 0.55f)));
				ImGui::Text("%u (%u)", row.priority, row.basePriority);
			}
			else
				ImGui::Text("%u", row.priority);

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(RtosTableModel::taskStateLabel(row.state));

			ImGui::TableNextColumn();

			if (!row.hasStack)
				ImGui::TextUnformatted(unknown());
			else
				ImGui::Text("%u", static_cast<unsigned>(row.stackBytes));

			/*
			 * The stack the task has touched, against the size of the stack. A
			 * free count on its own is a number nobody can judge: 184 bytes is
			 * comfortable in a 512 byte stack and a warning in a 256 byte one,
			 * and the size it belongs to is in the column beside it.
			 */
			ImGui::TableNextColumn();

			/* A stack close to full is what this column exists to find, so the
			   row says so twice: the cell is coloured, and a mark is put beside
			   the number. Colour on its own is not something every reader
			   has. */
			if (row.stackIsLow)
				ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImVec4(0.55f, 0.35f, 0.10f, 0.55f)));

			if (!row.hasStackWatermark)
				ImGui::TextUnformatted(unknown());
			else
				ImGui::Text("%s%u / %u (%u%%)%s", row.stackWatermarkReachedCap ? "<= " : "", static_cast<unsigned>(row.stackUsedBytes()),
				            static_cast<unsigned>(row.stackWatermarkStackBytes), static_cast<unsigned>(row.stackUsedPercent()), row.stackIsLow ? " !" : "");

			if (row.stackIsLow && ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::Text(I18n::tr("%u%% of this stack is still free, at or below the alert threshold of %u%%."), static_cast<unsigned>(row.stackFreePercent()),
				            static_cast<unsigned>(query.stackAlertFreePercent));
				ImGui::TextDisabled("%s", I18n::tr("The mark is on the used part, which is what the row shows."));
				ImGui::EndTooltip();
			}


			/*
			 * The share of the time this task used, worked out against what
			 * every task used over the same window. Its place in the column
			 * beside the stack pair is the plan's order: how big the task is,
			 * how much of its stack it uses, and how much of the time.
			 */
			ImGui::TableNextColumn();

			if (!row.hasCpuShare)
				ImGui::TextUnformatted(unknown());
			else
				ImGui::Text("%.1f%%", row.cpuSharePercent);

			/* Empty means it is not waiting on an object, which is a real
			   answer rather than a missing one, so it is left blank. */
			ImGui::TableNextColumn();

			if (!row.waitingOn.empty())
				ImGui::TextUnformatted(row.waitingOn.c_str());
		}

		ImGui::EndTable();
	}

	static const char* columnLabel(RtosTaskColumn column)
	{
		/* The order here is the order of the enum, so that a header can be
		   drawn from the column index without a second table to keep in step. */
		switch (column)
		{
			case RtosTaskColumn::Name:
				return I18n::tr("Name");

			case RtosTaskColumn::Address:
				return I18n::tr("Address");

			case RtosTaskColumn::Priority:
				return I18n::tr("Priority");

			case RtosTaskColumn::State:
				return I18n::tr("State");

			case RtosTaskColumn::StackSize:
				return I18n::tr("Stack");

			case RtosTaskColumn::StackWatermark:
				return I18n::tr("Stack used");


			case RtosTaskColumn::CpuLoad:
				/* Not "CPU": this is the share of the measured time this task used,
				   not the machine's utilisation. The column name has to say so
				   without a tooltip. */
				return I18n::tr("Task share");

			case RtosTaskColumn::WaitingOn:
				return I18n::tr("Waiting on");

			default:
				return "";
		}
	}

	static void drawHeaderRow(const std::shared_ptr<const RtosSnapshot>& snapshot)
	{
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

		for (int index = 0; index < 8; ++index)
		{
			ImGui::TableSetColumnIndex(index);

			const RtosTaskColumn column = static_cast<RtosTaskColumn>(index);
			const bool sorted = !query.problemFirst && query.sortColumn == column;

			if (sorted)
				ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImGuiCol_Header));

			const std::string label = std::string(columnLabel(column)) + (sorted ? (query.descending ? " v" : " ^") : "");

			if (ImGui::Selectable(label.c_str(), sorted))
			{
				/* Clicking the column that is already sorted turns it over
				   rather than leaving the click with nothing to show for it.
				   Clicking any column while the problem order is in force says
				   which order the reader wants instead - that choice ends the
				   default, and only the next session brings it back. */
				if (sorted)
					query.descending = !query.descending;
				else
				{
					query.problemFirst = false;
					query.sortColumn = column;
					query.descending = false;
				}
			}

			if (column == RtosTaskColumn::StackWatermark && ImGui::IsItemHovered())
				drawStackColumnTooltip(*snapshot);

			if (column == RtosTaskColumn::CpuLoad && ImGui::IsItemHovered())
				drawCpuColumnTooltip(*snapshot);
		}
	}

	/*
	 * The mark for a whole row this round did not read.
	 *
	 * A slash, and deliberately not the dash above. A dash is this round's
	 * answer about one field - it read the task and has nothing for that
	 * column; a slash is this round having no answer about the task at all.
	 * The row stays either way, because a task is not gone just because one
	 * walk of a list came back short.
	 */
	static const char* missingRow()
	{
		return "/";
	}

	static const char* unknown()
	{
		/* A dash, and not a zero. A zero is a reading; this is the absence of
		   one, and the two must not look the same in a column of numbers. */
		return "-";
	}

	/*
	 * The moment the reader pinned on the strip.
	 *
	 * The table answers "what is it doing now". A pinned moment answers "what
	 * was it doing then", which is what a reading that caught something
	 * interesting needs - and the sample is already in the strip's history, so
	 * this only says it in words.
	 */
	static void drawPinnedMoment(const RtosModel& model, const RtosSnapshot& snapshot)
	{
		ImGui::Separator();
		ImGui::TextUnformatted(I18n::tr("Pinned moment"));

		const bool pinned = GuiRtosPanelTimeline::getPinned();
		const double at = pinned ? GuiRtosPanelTimeline::getPinnedAt() : 0.0;

		/* Not `near`: Windows headers define that one as a macro, and the
		   declaration stops meaning anything. */
		std::vector<RtosTimeline::Sample> around;

		if (pinned)
			model.history().copyRange(at - 0.5, at + 0.5, around);

		const RtosTimeline::Sample* sample = around.empty() ? nullptr : &around[RtosTimeline::nearestIndex(around, at)];

		/*
		 * How to fill the block, on the heading's line, and always one line of
		 * it: nothing pinned yet, pinned, or pinned to a round the view has
		 * since dropped.
		 *
		 * On the heading's line rather than on one of its own, because the
		 * three rows under it are drawn whether or not they hold anything - and
		 * a sentence that came and went moved the pause box under this block by
		 * a line, so the reader aimed at it again every time a moment was
		 * pinned. A block with a hole in it is a block whose neighbours move.
		 */
		if (!pinned)
		{
			ImGui::SameLine(0.0f, 8.0f * GuiHelper::contentScale);
			GuiHelper::drawSecondaryText("%s", I18n::tr("Click the strip above to pin a moment."));
		}
		else if (sample == nullptr)
		{
			ImGui::SameLine(0.0f, 8.0f * GuiHelper::contentScale);
			GuiHelper::drawSecondaryText("%s", I18n::tr("That moment is no longer among the rounds the view keeps."));
		}

		uint32_t running = 0;
		uint32_t ready = 0;
		uint32_t blocked = 0;
		uint32_t suspended = 0;
		std::string runningName;

		if (sample != nullptr)
		{
			for (const RtosTimeline::Entry& entry : sample->entries)
			{
				switch (entry.state)
				{
					case RtosTaskState::Running:
						running++;

						/* The name comes from the round in hand: a task does not
						   change its name, and the sample carries only addresses. */
						if (runningName.empty())
						{
							const RtosTask* task = snapshot.find(entry.tcbAddress);

							if (task != nullptr)
								runningName = task->name;
						}

						break;

					case RtosTaskState::Ready:
						ready++;
						break;

					case RtosTaskState::Blocked:
						blocked++;
						break;

					case RtosTaskState::Suspended:
						suspended++;
						break;

					default:
						break;
				}
			}
		}

		/* The three rows are drawn whatever there is to put in them, and what
		   is missing is a dash. A dash is not a reading, and a row that is
		   there with a dash in it says "nothing was read"; a row that is not
		   there says nothing at all and moves the two under it. */
		const std::string missing = "-";

		char time[32] = {};

		if (sample != nullptr)
			std::snprintf(time, sizeof(time), "%+.2f s", sample->atSeconds);

		detailRow(I18n::tr("Time"), sample != nullptr ? std::string(time) : missing);

		/* A round can miss the running task. The counts below say what was
		   seen; this row says only which one it was. */
		detailRow(I18n::tr("Running"), runningName.empty() ? missing : runningName);

		char counts[128] = {};

		if (sample != nullptr)
			std::snprintf(counts, sizeof(counts), I18n::tr("%u running, %u ready, %u blocked, %u suspended"), running, ready, blocked, suspended);

		detailRow(I18n::tr("Tasks"), sample != nullptr ? std::string(counts) : missing);
	}

	/*
	 * A block whose height does not depend on what is in it.
	 *
	 * The right side is four blocks stacked in one window, and three of them
	 * change on their own: the objects come and go with the target, a pinned
	 * moment exists only after a click, a selected task only after a choice.
	 * Each change moved the block below it, so the pane shifted under the
	 * reader at moments they did not choose. Every block is now written into a
	 * fixed number of rows, and the leftover is filled with nothing.
	 *
	 * Filling with nothing is only half of it: a block that drew one row more
	 * than was reserved grew anyway, and everything under it moved. The box is
	 * a child window now, so its height is the height asked for whatever the
	 * block decides to draw, and a block that outgrows it scrolls instead of
	 * pushing. That mattered in practice - the pause box under the pinned
	 * moment sat under a different pixel depending on whether a moment had been
	 * pinned, and the reader had to aim again each time.
	 *
	 * The block is expected to draw its own heading: an empty block still says
	 * what it would have held.
	 */
	template <typename Block>
	static void fixedRows(float rows, const char* id, Block&& block)
	{
		const float height = ImGui::GetTextLineHeightWithSpacing() * rows;

		/* No padding inside the box: the blocks were drawn straight into the
		   pane before, and an inset here would put their headings a few pixels
		   right of every other heading in it. */
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::BeginChild(id, ImVec2(0.0f, height), false, ImGuiWindowFlags_NoBackground);
		ImGui::PopStyleVar();

		block();

		ImGui::EndChild();
	}

	/* One label and its value, with the values lined up in a column. */
	static void detailRow(const char* label, const std::string& value)
	{
		ImGui::TextUnformatted(label);
		ImGui::SameLine(120.0f * GuiHelper::contentScale);
		ImGui::TextUnformatted(value.c_str());
	}

	/* One line about a task, for the clipboard.
	 *
	 * The table is for comparing and the detail is for reading; what gets
	 * pasted into a bug report is neither. This is the form that stays
	 * readable in a message: name, address, state, the numbers that are worth
	 * quoting, and what it waits on. */
	static std::string taskSummary(const RtosTaskRow& row)
	{
		std::string summary = row.name.empty() ? std::string("?") : row.name;
		char text[192] = {};

		std::snprintf(text, sizeof(text), " 0x%08X %s", row.tcbAddress, RtosTableModel::taskStateLabel(row.state));
		summary += text;

		if (row.hasPriority)
		{
			if (row.hasBasePriority && row.basePriority != row.priority)
				std::snprintf(text, sizeof(text), " priority %u (was %u)", row.priority, row.basePriority);
			else
				std::snprintf(text, sizeof(text), " priority %u", row.priority);

			summary += text;
		}

		if (row.hasStackWatermark)
		{
			std::snprintf(text, sizeof(text), " stack %u/%u (%u%% used)", row.stackUsedBytes(), row.stackWatermarkStackBytes, row.stackUsedPercent());
			summary += text;
		}

		if (row.hasCpuShare)
		{
			std::snprintf(text, sizeof(text), " share %.1f%%", row.cpuSharePercent);
			summary += text;
		}

		if (!row.waitingOn.empty())
			summary += " waiting " + row.waitingOn;

		return summary;
	}

	/*
	 * The task the reader picked, in full.
	 *
	 * The table shows the same eight columns for every task, which is the right
	 * shape for comparing them and the wrong shape for reading one. How old the
	 * stack reading is, whether the priority shown is inherited, which of the
	 * kernel's lists the state came from, and what a share is a share of - all
	 * of that is a sentence, not a cell.
	 *
	 * The copy button is the same facts as one line: the table is for
	 * comparing, this is for reading, and neither is what gets pasted into a
	 * bug report.
	 */
	/*
	 * What the detail block is, and whether it is showing.
	 *
	 * The heading carries the task's name, so that a folded block still answers
	 * "which one is selected" - a closed section that says only its own name
	 * makes the reader open it to find out whether there is anything in it.
	 */
	/*
	 * The switch records, as rows.
	 *
	 * The strip answers "who was running when" as a picture, and a picture
	 * cannot be read for a number: a band's width is measured with the eye.
	 * This is the same data as rows - when a task was switched in, which task,
	 * and how long it stayed - so that the reader who needs "at 4.21 s, which
	 * one" can read it.
	 *
	 * Paused is a state of the view and not of the target: the rows stop being
	 * taken and the list stops following the end, so a row can be read while
	 * the machine keeps running. What arrived meanwhile is counted beside the
	 * box, because a list that silently stops is a list that lies.
	 */
	static void drawEvents(const RtosModel& model, const std::vector<RtosTaskRow>& rows)
	{
		const float scale = GuiHelper::contentScale;
		const RtosEventHistory& history = model.events();

		ImGui::Separator();

		if (ImGui::Checkbox(I18n::tr("pause"), &eventsPaused))
		{
			/* Leaving the pause has to rebuild, because while it was held the
			   list was deliberately left as it was. */
			if (!eventsPaused)
			{
				eventsHeld = 0;
				eventsCachedTotal = 0;
			}
		}

		ImGui::SameLine(0.0f, 8.0f * scale);
		ImGui::TextUnformatted(I18n::tr("Events"));

		/*
		 * Rebuilt when the history grew or when the question changed, and not
		 * every frame.
		 *
		 * A target that switches a thousand times a second keeps thousands of
		 * runs in ten seconds; copying all of them sixty times a second costs
		 * more than the view is worth. The list also holds only the last few
		 * hundred: rows that scroll past faster than they can be read are rows
		 * nobody reads, and the whole ten seconds is looked at on the strip.
		 *
		 * Narrowing to one task is a new question, so it rebuilds even while the
		 * list is paused: the reader asked for a different list, not for the one
		 * in front of them to keep still.
		 */
		const size_t total = history.size();
		const bool questionChanged = eventsTask != eventsCachedTask;

		if (questionChanged || (!eventsPaused && total != eventsCachedTotal))
		{
			std::vector<RtosEventRing::Interval> runs;
			history.copyRange(history.oldestSeconds(), history.newestSeconds(), runs);

			const RtosEventQuery::Selection selection = RtosEventQuery::select(runs, eventsTask, eventsLimit);

			eventsCache = std::move(selection.rows);
			eventsMatched = selection.matched;
			eventsCachedTotal = total;
			eventsCachedTask = eventsTask;
			eventsHeld = 0;
		}
		else if (eventsPaused && total > eventsCachedTotal)
			eventsHeld = static_cast<uint32_t>(total - eventsCachedTotal);

		/*
		 * What the list is limited to on the left, how to limit it on the
		 * right.
		 *
		 * A chip that says "only inv_low" beside a count that covers every task
		 * would be two answers to one question, so the two never share a place:
		 * while the list is narrowed, the chip with its cross sits where the
		 * question is asked; while it is not, the way in sits after the count,
		 * which is then about every task and reads that way.
		 */
		if (eventsTask != 0)
		{
			char chip[192] = {};
			std::snprintf(chip, sizeof(chip), I18n::tr("only %s"), eventTaskName(eventsTask, rows));

			const std::string label = std::string(chip) + " \xc3\x97##eventsOnlyOut";

			ImGui::SameLine(0.0f, 8.0f * scale);

			if (ImGui::SmallButton(label.c_str()))
				eventsTask = 0;

			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(I18n::tr("Show every task again."));
				ImGui::EndTooltip();
			}
		}

		ImGui::SameLine(0.0f, 8.0f * scale);

		if (eventsPaused && eventsHeld > 0)
			GuiHelper::drawSecondaryText(I18n::tr("%u new while paused"), eventsHeld);
		else if (eventsMatched > eventsCache.size())
			GuiHelper::drawSecondaryText(I18n::tr("last %u of %u runs"), static_cast<uint32_t>(eventsCache.size()),
			                             static_cast<uint32_t>(eventsMatched));
		else
			GuiHelper::drawSecondaryText(I18n::tr("%u runs"), static_cast<uint32_t>(eventsCache.size()));

		/* The way in, after the count it would change. Offered here rather than
		   only in the detail block because this is where the reader is looking
		   when the question "which of these are its" comes up. */
		if (eventsTask == 0 && selection().task != 0)
		{
			char chip[192] = {};
			std::snprintf(chip, sizeof(chip), I18n::tr("only %s"), eventTaskName(selection().task, rows));

			const std::string label = std::string(chip) + "##eventsOnlyIn";

			ImGui::SameLine(0.0f, 8.0f * scale);

			if (ImGui::SmallButton(label.c_str()))
				eventsTask = selection().task;

			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(I18n::tr("Show only this task's switches in the event list."));
				ImGui::EndTooltip();
			}
		}

		if (eventsCache.empty())
		{
			/* Three reasons and they are not the same: nothing was ever written
			   by the target, the reader has just opened the view, or the task
			   being looked at was never switched in. */
			if (eventsTask != 0)
				GuiHelper::drawSecondaryTextWrapped("%s", I18n::tr("This task has not been switched in during the runs the view keeps."));
			else
				GuiHelper::drawSecondaryTextWrapped("%s", I18n::tr("No switch records yet. The target writes them from its own timer interrupt, or sends them over SWO/RTT; the timeline's runs are the same records."));

			return;
		}

		const ImVec2 room = ImGui::GetContentRegionAvail();

		/* What is left, and no more.
		   A minimum of a few rows was asked for here, and asking for more than
		   is left is what made the pane scroll: with the content one line
		   taller than the window, the pane gained a scrollbar and everything in
		   it moved up by that line the moment a block above changed. The list
		   scrolls on its own, so a short list costs rows rather than a moving
		   pane. */
		ImGui::BeginChild("##rtosEventList", ImVec2(0.0f, std::max(room.y, 1.0f)), false,
		                  ImGuiWindowFlags_NoBackground);

		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;

		if (ImGui::BeginTable("rtosEvents", 3, flags))
		{
			ImGui::TableSetupColumn(I18n::tr("Time"), ImGuiTableColumnFlags_WidthFixed, 74.0f * scale);
			ImGui::TableSetupColumn(I18n::tr("Task"), ImGuiTableColumnFlags_WidthFixed, 150.0f * scale);
			ImGui::TableSetupColumn(I18n::tr("Ran for"), ImGuiTableColumnFlags_WidthFixed, 74.0f * scale);
			ImGui::TableHeadersRow();

			/* Oldest at the top, so that the list reads the way the strip does:
			   left to right, earlier to later. */
			for (size_t index = 0; index < eventsCache.size(); index++)
			{
				const RtosEventQuery::Row& run = eventsCache[index];

				ImGui::TableNextRow();

				/* The runs of the task the reader picked, tinted the way the
				   table tints a selected row, so that a name in the detail and a
				   row here are recognisably the same thing. */
				if (selection().task != 0 && run.tcbAddress == selection().task)
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header, 0.35f));

				ImGui::TableNextColumn();

				/* The row is the handle: clicking it pins the moment, which is
				   what the strip, the table and the detail all read - and picks
				   the task, so that a click in either half points both at the
				   same one. */
				char label[48] = {};
				std::snprintf(label, sizeof(label), "%.2f##run%u", run.fromSeconds, static_cast<unsigned>(index));

				if (ImGui::Selectable(label, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap))
				{
					GuiRtosPanelTimeline::pinAt(run.fromSeconds);
					selection().task = run.tcbAddress;
				}

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(eventTaskName(run.tcbAddress, rows));

				ImGui::TableNextColumn();
				GuiHelper::drawSecondaryText("%.1f ms", run.ranForSeconds() * 1000.0);
			}

			ImGui::EndTable();
		}

		if (!eventsPaused)
			ImGui::SetScrollY(ImGui::GetScrollMaxY());

		ImGui::EndChild();
	}

	/* The name a run belongs to: from the table's rows first, so that the list
	   and the lanes spell a task the same way. */
	static const char* eventTaskName(uint32_t address, const std::vector<RtosTaskRow>& rows)
	{
		for (const RtosTaskRow& row : rows)
		{
			if (row.tcbAddress == address && !row.name.empty())
				return row.name.c_str();
		}

		return unknown();
	}

	static void drawSelectedTaskSection(const RtosModel& model, const RtosSnapshot& snapshot,
	                                    const std::vector<RtosDiagnostics::Finding>& findings)
	{
		const float scale = GuiHelper::contentScale;

		ImGui::Separator();

		if (ImGui::ArrowButton("##rtosSelectedTaskFold", selectedTaskOpen ? ImGuiDir_Down : ImGuiDir_Right))
			selectedTaskOpen = !selectedTaskOpen;

		ImGui::SameLine(0.0f, 6.0f * scale);
		ImGui::TextUnformatted(I18n::tr("Selected task"));

		if (selection().task != 0)
		{
			const RtosTask* task = snapshot.find(selection().task);

			if (task != nullptr && !task->name.empty())
			{
				ImGui::SameLine(0.0f, 6.0f * scale);
				GuiHelper::drawSecondaryText("· %s", task->name.c_str());
			}
		}

		if (!selectedTaskOpen)
			return;

		const ImVec2 room = ImGui::GetContentRegionAvail();

		/* What is left, and no more: see the note on the event list, which is
		   the same block on the other side of the split. */
		ImGui::BeginChild("##rtosSelectedTask", ImVec2(0.0f, std::max(room.y, 1.0f)), false,
		                  ImGuiWindowFlags_NoBackground);

		drawSelectedTask(model, snapshot, findings);

		ImGui::EndChild();
	}

	static void drawSelectedTask(const RtosModel& model, const RtosSnapshot& snapshot, const std::vector<RtosDiagnostics::Finding>& findings)
	{
		const float scale = GuiHelper::contentScale;

		if (selection().task == 0)
		{
			GuiHelper::drawSecondaryTextWrapped("%s", I18n::tr("Click a row in the table, a finding or a lane to read one task here."));

			return;
		}

		/* Built with an empty query: the detail is about the task, not about
		   whether the current filter would let it into the table. */
		const RtosTaskTableQuery unfiltered;
		const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(snapshot, unfiltered);
		const auto found = std::find_if(rows.begin(), rows.end(), [](const RtosTaskRow& row) { return row.tcbAddress == selection().task; });

		if (found == rows.end())
		{
			GuiHelper::drawSecondaryTextWrapped("%s", I18n::tr("The task that was selected is not in this round."));

			return;
		}

		const RtosTaskRow& row = *found;

		ImGui::SameLine(0.0f, 12.0f * scale);

		if (ImGui::SmallButton(I18n::tr("copy")))
			ImGui::SetClipboardText(taskSummary(row).c_str());

		if (ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
			ImGui::TextUnformatted(taskSummary(row).c_str());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		char address[16] = {};
		std::snprintf(address, sizeof(address), "0x%08X", row.tcbAddress);

		detailRow(I18n::tr("Name"), row.name.empty() ? unknown() : row.name);
		detailRow(I18n::tr("Address"), address);
		detailRow(I18n::tr("State"), RtosTableModel::taskStateLabel(row.state));
		detailRow(I18n::tr("Found on"), row.sourceList.empty() ? unknown() : row.sourceList);

		if (!row.hasPriority)
			detailRow(I18n::tr("Priority"), unknown());
		else if (row.hasBasePriority && row.basePriority != row.priority)
		{
			char text[48] = {};
			std::snprintf(text, sizeof(text), I18n::tr("%u (was %u) - inherited while a lock is held"), row.priority, row.basePriority);
			detailRow(I18n::tr("Priority"), text);
		}
		else
			detailRow(I18n::tr("Priority"), std::to_string(row.priority));

		if (!row.hasStackWatermark)
			detailRow(I18n::tr("Stack"), unknown());
		else
		{
			char text[128] = {};
			std::snprintf(text, sizeof(text), I18n::tr("%s%u / %u B, %u%% free - read %.1f s ago"), row.stackWatermarkReachedCap ? "<= " : "",
			              row.stackUsedBytes(), row.stackWatermarkStackBytes, row.stackFreePercent(), row.stackWatermarkAgeSeconds);
			detailRow(I18n::tr("Stack"), text);
		}

		if (!row.hasCpuShare)
			detailRow(I18n::tr("Task share"), unknown());
		else
		{
			char text[128] = {};
			std::snprintf(text, sizeof(text), I18n::tr("%.1f%% of the time the tasks used, over the last %.2f s"), row.cpuSharePercent,
			              snapshot.cpuLoad.windowSeconds);
			detailRow(I18n::tr("Task share"), text);
		}

		detailRow(I18n::tr("Waiting on"), row.waitingOn.empty() ? std::string("-") : row.waitingOn);

		/*
		 * Which findings this task is caught in.
		 *
		 * The row is in the table whatever reason put it there, and two of the
		 * four reasons leave no mark on the row itself: a task on a deadlock
		 * ring is blocked like any other, and a ready task the scheduler keeps
		 * passing over looks ready. Without this a reader who filtered to the
		 * problems would have to guess why a plain looking row survived.
		 */
		const std::vector<RtosDiagnostics::Finding> involvement = RtosDiagnostics::findingsFor(findings, row.tcbAddress);

		if (involvement.empty())
			detailRow(I18n::tr("Problems"), std::string("-"));
		else
		{
			std::string problems;

			for (const RtosDiagnostics::Finding& finding : involvement)
			{
				if (!problems.empty())
					problems += ", ";

				problems += findingLabel(finding.kind);

				/* How long it has held, where the finding measured it. A
				   deadlock is a shape, not a duration. */
				if (finding.heldSeconds > 0.0)
				{
					char held[32] = {};
					std::snprintf(held, sizeof(held), I18n::tr("%.1f s"), finding.heldSeconds);
					problems += " ";
					problems += held;
				}
			}

			detailRow(I18n::tr("Problems"), problems);
		}

		/* When it last had the processor. Thirty seconds back is what the query
		   asks the history for: further than that and "not in the last 30 s" is
		   the honest answer for a task that is not running. */
		const double newest = model.history().newestSeconds();

		if (row.state == RtosTaskState::Running)
			detailRow(I18n::tr("Last ran"), I18n::tr("right now"));
		else
		{
			std::vector<RtosTimeline::Sample> recent;
			model.history().copyRange(std::max(0.0, newest - 30.0), newest, recent);

			const double lastRun = RtosTimeline::lastRunningAt(recent, row.tcbAddress);

			if (lastRun >= 0.0)
			{
				char elapsed[48] = {};
				std::snprintf(elapsed, sizeof(elapsed), I18n::tr("%.2f s ago"), newest - lastRun);

				detailRow(I18n::tr("Last ran"), elapsed);
			}
			else
			{
				char never[64] = {};
				std::snprintf(never, sizeof(never), I18n::tr("not in the last %.0f s"), 30.0);

				detailRow(I18n::tr("Last ran"), never);
			}
		}

		/*
		 * When it was put on the processor, as rows.
		 *
		 * "Last ran 2.13 s ago" answers when, and nothing about how often or for
		 * how long. These are the same switch records the list below shows,
		 * narrowed to this task, and each row is a jump: a detail that raised a
		 * question can be followed without hunting for the row in a list of six
		 * hundred.
		 *
		 * Five seconds and three rows, because this is a detail block and not a
		 * second list: enough to see whether the task is being switched in at
		 * all, and not enough to read instead of the list.
		 */
		const double switchFrom = std::max(0.0, newest - 5.0);

		std::vector<RtosEventRing::Interval> switchRuns;
		model.events().copyRange(switchFrom, newest, switchRuns);

		const RtosEventQuery::Selection switches = RtosEventQuery::select(switchRuns, row.tcbAddress, 3);

		ImGui::TextUnformatted(I18n::tr("Switches (5 s)"));
		ImGui::SameLine(120.0f * scale);
		ImGui::TextUnformatted(switches.matched == 0 ? "-" : std::to_string(switches.matched).c_str());

		for (const RtosEventQuery::Row& switchRun : switches.rows)
		{
			char label[96] = {};
			std::snprintf(label, sizeof(label), "    %s %.2f s, %.1f ms##switch%.2f", I18n::tr("at"), switchRun.fromSeconds,
			              switchRun.ranForSeconds() * 1000.0, switchRun.fromSeconds);

			if (ImGui::Selectable(label))
				GuiRtosPanelTimeline::pinAt(switchRun.fromSeconds);
		}
	}

	/*
	 * The synchronisation objects, under the task table.
	 *
	 * A kernel with no queue registry keeps no list of its queues at all, so
	 * there is nothing to enumerate and that is not a failure to report - the
	 * panel says so and stops. The counts come from the item count of the two
	 * wait lists rather than from walking them, which is what makes this cost
	 * nothing beyond the read of each registry entry.
	 */
	static void drawSyncObjects(const RtosSnapshot& snapshot)
	{
		ImGui::Separator();
		ImGui::TextUnformatted(I18n::tr("Synchronisation objects"));

		if (!snapshot.queueRegistryAvailable)
		{
			ImGui::TextDisabled("%s", I18n::tr("This kernel keeps no registry of its queues."));

			return;
		}

		if (snapshot.syncObjects.empty())
		{
			ImGui::TextDisabled("%s", I18n::tr("None are registered."));

			return;
		}

		/* No sideways scrolling here, and stretching columns instead: this
		   table is in the wide half of the view now, so the room is there to
		   share out. The two do not go together - with `ScrollX` a table is
		   sized to its contents, and the last column of it ends up short. */
		const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
		                              ImGuiTableFlags_ContextMenuInBody;

		/* As tall as what is in it, within what the strip above has left. A
		   table given no height with `ScrollY` takes everything that remains,
		   which would put the objects under the bottom of the window. */
		const float objectRowHeight = ImGui::GetTextLineHeightWithSpacing();
		constexpr size_t objectRows = 4;
		const float objectHeight = ImGui::GetFrameHeight() + objectRowHeight * static_cast<float>(objectRows) + ImGui::GetStyle().CellPadding.y * 2.0f;
		const ImVec2 objectSize(0.0f, objectHeight);

		if (!ImGui::BeginTable("rtosSyncObjects-v2", 6, flags, objectSize))
			return;

		ImGui::TableSetupScrollFreeze(0, 1);

		/* The weights follow how much each column holds: two short names, an
		   address, a pair of counts, and the two headers that were being cut -
		   "Items / capacity" and "Waiting to receive" - which get the most. */
		ImGui::TableSetupColumn(I18n::tr("Kind"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn(I18n::tr("Name"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn(I18n::tr("Address"), ImGuiTableColumnFlags_WidthStretch, 1.1f);
		ImGui::TableSetupColumn(I18n::tr("Items / capacity"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableSetupColumn(I18n::tr("Waiting to send"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
		ImGui::TableSetupColumn(I18n::tr("Waiting to receive"), ImGuiTableColumnFlags_WidthStretch, 1.8f);

		/* Draws the header row from the labels above, so the column order
		   lives in one place instead of two. */
		ImGui::TableHeadersRow();
		for (const RtosSyncObject& object : snapshot.syncObjects)
		{
			ImGui::TableNextRow();

			/* The object the selected task is waiting on, so that a row in the
			   task table leads somewhere in this one. */
			const bool objectSelected = !selection().object.empty() && object.name == selection().object;

			if (objectSelected)
			{
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header, 0.55f));

				if (selection().object != selection().scrolledObject)
				{
					ImGui::SetScrollHereY(0.5f);
					selection().scrolledObject = selection().object;
				}
			}

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(toString(object.kind));

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(object.name.empty() ? unknown() : object.name.c_str());

			ImGui::TableNextColumn();
			ImGui::Text("0x%08X", object.address);

			ImGui::TableNextColumn();
			if (!object.hasCounts)
				ImGui::TextUnformatted(unknown());
			else
				ImGui::Text("%u / %u", object.items, object.capacity);

			ImGui::TableNextColumn();
			if (!object.hasWaiters)
				ImGui::TextUnformatted(unknown());
			else
				ImGui::Text("%u", object.waitingToSend);

			ImGui::TableNextColumn();
			if (!object.hasWaiters)
				ImGui::TextUnformatted(unknown());
			else
				ImGui::Text("%u", object.waitingToReceive);
		}

		ImGui::EndTable();
	}
};

#endif
