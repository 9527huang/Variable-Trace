#ifndef _GUIRTOSPANEL_HPP
#define _GUIRTOSPANEL_HPP

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "GuiHelper.hpp"
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
 * reason worth writing down. CPU load and context switch counts need the
 * target built with `configGENERATE_RUN_TIME_STATS`; the stack water mark needs
 * the kernel asked for it, which the task view does not do yet; the object a
 * task waits on needs the wait lists chased back to whatever owns them. None of
 * them can be filled in from what a round reads today, and a column filled with
 * zeroes would be a claim about the target rather than a gap in the reading.
 */
class GuiRtosPanel
{
   public:
	static void draw(const std::shared_ptr<RtosModel>& model, const std::string& explanation)
	{
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

		drawInconsistencyNotice(*snapshot);
		drawFilters(snapshot);
		drawTable(snapshot);
		drawSyncObjects(*snapshot);
	}

	/*
	 * What the snapshot is and is not.
	 *
	 * The round walks eight kernel lists one after another while the target
	 * moves tasks between them, so the parts of it were not all taken at the
	 * same instant. That is not a defect to hide - it is the honest answer to
	 * "when was this true", and it belongs on screen rather than in a log.
	 */
	static void drawInconsistencyNotice(const RtosSnapshot& snapshot)
	{
		if (snapshot.inconsistencies.empty())
			return;

		ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::orangeLight());
		ImGui::Text(I18n::tr("Parts of this table were not read at one instant: %d place(s) caught the target moving."),
		            static_cast<int>(snapshot.inconsistencies.size()));
		ImGui::PopStyleColor();

		if (ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();

			for (const std::string& note : snapshot.inconsistencies)
				ImGui::TextUnformatted(note.c_str());

			ImGui::EndTooltip();
		}

		ImGui::Separator();
	}

   private:
	/* The filters live as long as the program, because they are the user's
	   choice rather than the round's data. */
	inline static RtosTaskTableQuery query{};
	inline static bool showOnlyBlocked = false;

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

	static void drawFilters(const std::shared_ptr<const RtosSnapshot>& snapshot)
	{
		if (!searchBufferPrimed)
		{
			std::snprintf(searchBuffer, sizeof(searchBuffer), "%s", query.nameSearch.c_str());
			searchBufferPrimed = true;
		}

		ImGui::SetNextItemWidth(180 * GuiHelper::contentScale);

		if (ImGui::InputTextWithHint("##rtosNameSearch", I18n::tr("search by name"), searchBuffer, sizeof(searchBuffer)))
			query.nameSearch = searchBuffer;

		ImGui::SameLine();

		if (ImGui::Checkbox(I18n::tr("only blocked"), &showOnlyBlocked))
			query.states = showOnlyBlocked ? std::vector<RtosTaskState>{RtosTaskState::Blocked} : std::vector<RtosTaskState>{};

		ImGui::SameLine();

		if (ImGui::Checkbox(I18n::tr("priority floor"), &query.hasPriorityFloor))
		{
			/* Turning the floor on with nothing set would hide every task
			   that has a priority, which reads as a broken table rather than
			   as a filter someone asked for. */
			if (query.hasPriorityFloor && query.priorityFloor == 0)
				query.priorityFloor = 1;
		}

		if (query.hasPriorityFloor)
		{
			ImGui::SameLine();
			ImGui::SetNextItemWidth(80 * GuiHelper::contentScale);

			int floor = static_cast<int>(query.priorityFloor);

			if (ImGui::InputInt("##rtosFloor", &floor, 0, 0))
				query.priorityFloor = floor;
		}

		ImGui::SameLine();
		ImGui::Text(I18n::tr("%d of %d shown"), static_cast<int>(RtosTableModel::taskRows(*snapshot, query).size()),
		            static_cast<int>(snapshot->taskCount()));

		ImGui::Separator();
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
		const ImVec2 size(0.0f, ImGui::GetContentRegionAvail().y * 0.6f);

		if (!ImGui::BeginTable("rtosTasks", 6, flags, size))
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
		ImGui::TableSetupColumn(columnLabel(RtosTaskColumn::WaitingOn));

		drawHeaderRow();

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
			ImGui::TextUnformatted(toString(row.state));

			ImGui::TableNextColumn();

			if (!row.hasStack)
				ImGui::TextUnformatted(unknown());
			else
				ImGui::Text("%u", static_cast<unsigned>(row.stackBytes));

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

			case RtosTaskColumn::WaitingOn:
				return I18n::tr("Waiting on");

			default:
				return "";
		}
	}

	static void drawHeaderRow()
	{
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

		for (int index = 0; index < 6; ++index)
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
