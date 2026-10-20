#ifndef _GUIRTOSPANEL_HPP
#define _GUIRTOSPANEL_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "GuiHelper.hpp"
#include "GuiRtosPanelTimeline.hpp"
#include "GuiRtosSelection.hpp"
#include "RtosDiagnostics.hpp"
#include "I18n.hpp"
#include "RtosModel.hpp"
#include "RtosSyncEnumerator.hpp"
#include "RtosTableModel.hpp"
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
	static void draw(const std::shared_ptr<RtosModel>& model, const std::string& explanation, uint32_t stackAlertFreePercent, RtosViewSettings& view)
	{
		/* What the reader had set up is taken once, from the settings file, so
		   that a run does not change under a number someone is moving in a box -
		   and then written back every frame, so that what they set up in this
		   run is what the next one opens with. */
		if (!stackAlertSeeded)
		{
			query.sortColumn = static_cast<RtosTaskColumn>(view.sortColumn);
			query.descending = view.sortDescending;
			query.hasPriorityFloor = view.hasPriorityFloor;
			query.priorityFloor = view.priorityFloor;
			query.onlyStackAlerts = view.onlyStackAlerts;
			query.stackAlertFreePercent = stackAlertFreePercent;

			query.states = view.showOnlyBlocked ? std::vector<RtosTaskState>{RtosTaskState::Blocked} : std::vector<RtosTaskState>{};

			GuiRtosPanelTimeline::setWindowSeconds(view.timelineWindowSeconds);
			GuiRtosPanelTimeline::setFollowPlotCursors(view.followPlotCursors);
			GuiRtosPanelTimeline::setFollowTableOrder(view.timelineFollowsTable);

			stackAlertSeeded = true;
		}

		view.sortColumn = static_cast<uint32_t>(query.sortColumn);
		view.sortDescending = query.descending;
		view.hasPriorityFloor = query.hasPriorityFloor;
		view.priorityFloor = query.priorityFloor;
		view.onlyStackAlerts = query.onlyStackAlerts;
		view.showOnlyBlocked = query.states.size() == 1 && query.states.front() == RtosTaskState::Blocked;
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

		const std::shared_ptr<const RtosSnapshot> snapshot = model->current();

		if (snapshot == nullptr)
		{
			ImGui::TextUnformatted(I18n::tr("No round has been published yet."));

			return;
		}

		drawConsistencyLine(*snapshot);
		drawStackScanNotice(*snapshot);
		drawCpuLoadNotice(*snapshot);

		/* The findings are worked out before the summary, because the summary
		   quotes the longest wait from them: two readers of one answer rather
		   than two computations that could disagree. */
		const std::vector<RtosDiagnostics::Finding> findings = RtosDiagnostics::diagnose(*snapshot, model->history());

		fixedRows(2.0f, [&] { drawSummary(*snapshot, findings); });

		/* Findings, and only when there are any: a section that says "nothing
		   is wrong" every round trains the reader to skip the place where the
		   answer appears. */

		if (!findings.empty())
			drawDiagnostics(findings);

		drawFilters(snapshot);

		drawTable(snapshot);
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

		/* Which order the rows go in: the stable one, or the one the reader
		   sorted the table to. The stable order exists because a lane that
		   moves is a lane that cannot be followed; the table's order exists
		   because a reader comparing a row with a lane wants them adjacent. */
		const std::vector<uint32_t> lanes = GuiRtosPanelTimeline::getFollowTableOrder()
		                                        ? RtosTableModel::addressesOf(RtosTableModel::taskRows(*snapshot, query))
		                                        : RtosTableModel::laneOrder(*snapshot);

		GuiRtosPanelTimeline::draw(*model, *snapshot, plotCursors, lanes, selection());

		/* Under the strip, and in the same window: a task that is waiting is
		   waiting for one of these, so the two are read together. */
		/* Seven rows: a heading, a table header and four objects. Twelve for the
		   detail, which is the tallest it gets on a task with everything read.
		   Four for the pinned moment. What they do not use is left blank. */
		fixedRows(7.0f, [&] { drawSyncObjects(*snapshot); });
		fixedRows(4.0f, [&] { drawPinnedMoment(*model, *snapshot); });

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
		{
			const ImVec2 room = ImGui::GetContentRegionAvail();

			ImGui::BeginChild("##rtosSelectedTask", ImVec2(0.0f, std::max(room.y, ImGui::GetTextLineHeightWithSpacing() * 4.0f)), false,
			                  ImGuiWindowFlags_NoBackground);

			drawSelectedTask(*model, *snapshot);

			ImGui::EndChild();
		}
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

			const char* label = deadlock ? "suspected deadlock"
			                              : (starvation ? "ready, never switched in" : (unbounded ? "waiting with no timeout" : "priority inheritance"));

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

		/* The search field draws from its own buffer, so clearing the query alone
		   would leave the old text in the box. */
		std::snprintf(searchBuffer, sizeof(searchBuffer), "%s", "");
	}

	/* Whether any filter is narrowing the table. Used to decide whether the
	   button that clears them is worth pressing, so that it does not sit there
	   lit up with nothing to do. */
	static bool filterIsOn()
	{
		return !query.nameSearch.empty() || !query.states.empty() || query.onlyStackAlerts || query.hasPriorityFloor;
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
	 * What narrows the table, in two rows: what a reader changes while looking
	 * at it, and the two thresholds that are set once.
	 *
	 * Every number carries its unit and every condition reads as a sentence
	 * now. "stack alert at 20" was twenty of something the reader had to guess
	 * at, and the button that resets the sort sat among the filters although it
	 * is a table setting - the header already sorts, and clicking the sorted
	 * column turns it over, so the button is gone.
	 */
	static void drawFilters(const std::shared_ptr<const RtosSnapshot>& snapshot)
	{
		const float scale = GuiHelper::contentScale;

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
		char shown[64] = {};
		std::snprintf(shown, sizeof(shown), I18n::tr("%d of %d shown"), static_cast<int>(RtosTableModel::taskRows(*snapshot, query).size()),
		              static_cast<int>(snapshot->taskCount()));

		const float shownWidth = ImGui::CalcTextSize(shown).x;

		ImGui::SameLine();

		const float remaining = ImGui::GetContentRegionAvail().x;

		if (remaining > shownWidth + 16.0f * scale)
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + remaining - shownWidth);

		GuiHelper::drawSecondaryText("%s", shown);

		ImGui::Separator();
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

	static void drawTable(const std::shared_ptr<const RtosSnapshot>& snapshot)
	{
		const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(*snapshot, query);

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
		const ImVec2 size(0.0f, ImGui::GetContentRegionAvail().y);

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
			const bool sorted = query.sortColumn == column;

			if (sorted)
				ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImGuiCol_Header));

			const std::string label = std::string(columnLabel(column)) + (sorted ? (query.descending ? " v" : " ^") : "");

			if (ImGui::Selectable(label.c_str(), sorted))
			{
				/* Clicking the column that is already sorted turns it over
				   rather than leaving the click with nothing to show for it. */
				if (sorted)
					query.descending = !query.descending;
				else
				{
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

		/* The heading stays when there is no moment: the block keeps its room,
		   and the sentence says how to fill it. */
		if (!GuiRtosPanelTimeline::getPinned())
		{
			GuiHelper::drawSecondaryText("%s", I18n::tr("Click the strip above to pin a moment."));

			return;
		}

		const double at = GuiRtosPanelTimeline::getPinnedAt();

		/* Not `near`: Windows headers define that one as a macro, and the
		   declaration stops meaning anything. */
		std::vector<RtosTimeline::Sample> around;
		model.history().copyRange(at - 0.5, at + 0.5, around);

		if (around.empty())
		{
			GuiHelper::drawSecondaryTextWrapped("%s", I18n::tr("That moment is no longer among the rounds the view keeps."));

			return;
		}

		const RtosTimeline::Sample& sample = around[RtosTimeline::nearestIndex(around, at)];

		uint32_t running = 0;
		uint32_t ready = 0;
		uint32_t blocked = 0;
		uint32_t suspended = 0;
		std::string runningName;

		for (const RtosTimeline::Entry& entry : sample.entries)
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

		char time[32] = {};
		std::snprintf(time, sizeof(time), "%+.2f s", sample.atSeconds);

		detailRow(I18n::tr("Time"), time);

		/* Only when there is one: a round can miss the running task, and a
		   dash under a heading that says "Running" reads as a broken reading
		   rather than as a round that did not see it. The counts below say what
		   was seen. */
		if (!runningName.empty())
			detailRow(I18n::tr("Running"), runningName);

		char counts[128] = {};
		std::snprintf(counts, sizeof(counts), I18n::tr("%u running, %u ready, %u blocked, %u suspended"), running, ready, blocked, suspended);
		detailRow(I18n::tr("Tasks"), counts);
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
	 * The block is expected to draw its own heading: an empty block still says
	 * what it would have held.
	 */
	template <typename Block>
	static void fixedRows(float rows, Block&& block)
	{
		const float top = ImGui::GetCursorPosY();

		block();

		const float height = ImGui::GetTextLineHeightWithSpacing() * rows;
		const float used = ImGui::GetCursorPosY() - top;

		if (used < height)
			ImGui::Dummy(ImVec2(0.0f, height - used));
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
	static void drawSelectedTask(const RtosModel& model, const RtosSnapshot& snapshot)
	{
		const float scale = GuiHelper::contentScale;

		ImGui::Separator();
		ImGui::TextUnformatted(I18n::tr("Selected task"));

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
