#ifndef _GUIRTOSPANEL_HPP
#define _GUIRTOSPANEL_HPP

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "GuiHelper.hpp"
#include "GuiRtosPanelTimeline.hpp"
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
	static void draw(const std::shared_ptr<RtosModel>& model, const std::string& explanation, uint32_t stackAlertFreePercent)
	{
		/* The threshold comes from the settings file, and is taken once so that
		   a run does not change under a number someone is moving in the box. */
		if (!stackAlertSeeded)
		{
			query.stackAlertFreePercent = stackAlertFreePercent;
			stackAlertSeeded = true;
		}

		if (model == nullptr)
		{
			/* The reason is shown when there is one. An empty panel that says
			   only "nothing here" leaves the reader to work out whether the
			   switch is off, the acquisition is stopped, or the project open
			   is not the one built with the kernel in it - and the last of
			   those is the one nobody guesses. */
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

		drawConsistencyLine(*snapshot);
		drawStackScanNotice(*snapshot);
		drawCpuLoadNotice(*snapshot);
		drawSummary(*snapshot);
		drawFilters(snapshot);

		/* The table keeps the larger share and the strip takes a fixed slice of
		   what is left, so that a short window does not push the objects out of
		   sight entirely. */
		drawTable(snapshot);
		GuiRtosPanelTimeline::draw(*model, *snapshot);
		drawSyncObjects(*snapshot);
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

		ImGui::TextDisabled("%s", I18n::tr("The target does not fill its stacks, so how much of one is used cannot be worked out. FreeRTOS fills them when configCHECK_FOR_STACK_OVERFLOW is above 1, or configUSE_TRACE_FACILITY, or INCLUDE_uxTaskGetStackHighWaterMark, or INCLUDE_uxTaskGetStackHighWaterMark2 is set."));

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
			ImGui::TextDisabled("%s", I18n::tr("The target was not built with run time statistics (configGENERATE_RUN_TIME_STATS), so how much of the time each task used cannot be worked out."));

			ImGui::Separator();

			return;
		}

		if (snapshot.cpuLoad.tasksMeasured > 0 && !snapshot.cpuLoad.counting)
		{
			ImGui::TextDisabled("%s", I18n::tr("The run time counter did not move over the last window, so the shares cannot be worked out. The timer behind it is probably not running."));

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
	static void drawSummary(const RtosSnapshot& snapshot)
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

			ImGui::TextDisabled("%u %s", summary.inherited, I18n::tr("priority inherited"));

			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(I18n::tr("A task whose current priority is above its base one is holding a lock that a higher priority task is waiting for."));
				ImGui::EndTooltip();
			}
		}

		ImGui::Separator();
	}

	/* A count that is also a filter: clicking it narrows the table to those rows,
	   clicking it again lets them all back. */
	static void stateChip(uint32_t count, RtosTaskState state)
	{
		const bool active = query.states.size() == 1 && query.states.front() == state;

		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

		const std::string label = std::to_string(count) + " " + RtosTableModel::taskStateLabel(state);

		if (ImGui::SmallButton(label.c_str()))
		{
			if (active)
				query.states.clear();
			else
				query.states = {state};
		}

		if (active)
			ImGui::PopStyleColor();
	}

	static void drawFilters(const std::shared_ptr<const RtosSnapshot>& snapshot)
	{
		const float scale = GuiHelper::contentScale;

		/* Row one: what a reader changes while looking at the table. */
		if (!searchBufferPrimed)
		{
			std::snprintf(searchBuffer, sizeof(searchBuffer), "%s", query.nameSearch.c_str());
			searchBufferPrimed = true;
		}

		ImGui::SetNextItemWidth(180 * scale);

		if (ImGui::InputTextWithHint("##rtosNameSearch", I18n::tr("search by name"), searchBuffer, sizeof(searchBuffer)))
			query.nameSearch = searchBuffer;

		/* The checkbox and the chips both drive one filter, so its state is read
		   from the filter rather than kept beside it: two records would let the
		   box show clear while the table was still narrowed. */
		sameLineIfFits(110.0f * scale);

		bool onlyBlocked = query.states.size() == 1 && query.states.front() == RtosTaskState::Blocked;

		if (ImGui::Checkbox(I18n::tr("only blocked"), &onlyBlocked))
			query.states = onlyBlocked ? std::vector<RtosTaskState>{RtosTaskState::Blocked} : std::vector<RtosTaskState>{};

		sameLineIfFits(140.0f * scale);

		if (ImGui::Checkbox(I18n::tr("stack alerts only"), &query.onlyStackAlerts))
		{
			/* The flag is the filter; there is nothing else to keep in step. */
		}

		sameLineIfFits(130.0f * scale);

		if (ImGui::Button(I18n::tr("clear filters")))
			clearFilters();

		/* Row two: the settings a reader changes once. No `SameLine` before the
		   first of them, so they start a line of their own. */
		drawAlertThreshold();

		sameLineIfFits(120.0f * scale);

		if (ImGui::Checkbox(I18n::tr("priority floor"), &query.hasPriorityFloor))
		{
			/* Turning the floor on with nothing set would hide every task that
			   has a priority, which reads as a broken table rather than as a
			   filter someone asked for. */
			if (query.hasPriorityFloor && query.priorityFloor == 0)
				query.priorityFloor = 1;
		}

		if (query.hasPriorityFloor)
		{
			sameLineIfFits(90.0f * scale);
			ImGui::SetNextItemWidth(80 * scale);

			int floor = static_cast<int>(query.priorityFloor);

			if (ImGui::InputInt("##rtosFloor", &floor, 0, 0))
				query.priorityFloor = floor;
		}

		sameLineIfFits(130.0f * scale);

		if (ImGui::Button(I18n::tr("reset sort")))
		{
			query.sortColumn = RtosTaskColumn::Name;
			query.descending = false;
		}

		sameLineIfFits(150.0f * scale);
		ImGui::Text(I18n::tr("%d of %d shown"), static_cast<int>(RtosTableModel::taskRows(*snapshot, query).size()),
		            static_cast<int>(snapshot->taskCount()));

		ImGui::Separator();
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
		ImGui::TextUnformatted(I18n::tr("stack alert at"));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(70 * GuiHelper::contentScale);

		int percent = static_cast<int>(query.stackAlertFreePercent);

		if (ImGui::InputInt("##rtosStackAlert", &percent, 0, 0))
			query.stackAlertFreePercent = percent < 0 ? 0u : static_cast<uint32_t>(percent);

		if (ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(I18n::tr("A task whose stack is this empty or less is marked. Zero turns the marking off, and a task with no reading is never marked."));
			ImGui::EndTooltip();
		}

		sameLineIfFits(140.0f * GuiHelper::contentScale);
	}

	static void drawTable(const std::shared_ptr<const RtosSnapshot>& snapshot)
	{
		const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(*snapshot, query);

		const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

		/*
		 * The table is given a height rather than the whole window.
		 *
		 * With `ScrollY` and no height it takes everything that is left, which
		 * pushed the synchronisation objects below it clean out of the window
		 * - they were drawn, and drawn out of sight. The share is taken of
		 * what is available at this point, so the two tables between them fill
		 * the view whatever its size.
		 */
		const ImVec2 size(0.0f, ImGui::GetContentRegionAvail().y * 0.45f);

		if (!ImGui::BeginTable("rtosTasks", 8, flags, size))
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
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::Name));
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::Address));
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::Priority));
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::State));
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::StackSize));
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::StackWatermark));
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::CpuLoad));
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::WaitingOn));

			drawHeaderRow(snapshot);

		for (const RtosTaskRow& row : rows)
		{
			ImGui::TableNextRow();

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(row.name.empty() ? unknown() : row.name.c_str());

			ImGui::TableNextColumn();
			ImGui::Text("0x%08X", row.tcbAddress);

			/* Priority: the base value is shown beside the current one when
			   they differ, because that difference is the whole visible sign
			   of priority inheritance and is otherwise invisible. */
			ImGui::TableNextColumn();

			if (!row.hasPriority)
				ImGui::TextUnformatted(unknown());
			else if (row.basePriority != row.priority)
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
				return I18n::tr("CPU");

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

		const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

		if (!ImGui::BeginTable("rtosSyncObjects", 6, flags))
			return;

		ImGui::TableSetupScrollFreeze(0, 1);

		ImGui::TableSetupColumn(I18n::tr("Kind"));
		ImGui::TableSetupColumn(I18n::tr("Name"));
		ImGui::TableSetupColumn(I18n::tr("Address"));
		ImGui::TableSetupColumn(I18n::tr("Items / capacity"));
		ImGui::TableSetupColumn(I18n::tr("Waiting to send"));
		ImGui::TableSetupColumn(I18n::tr("Waiting to receive"));

		/* Draws the header row from the labels above, so the column order
		   lives in one place instead of two. */
		ImGui::TableHeadersRow();
		for (const RtosSyncObject& object : snapshot.syncObjects)
		{
			ImGui::TableNextRow();

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
