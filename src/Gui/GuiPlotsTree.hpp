#pragma once

#include <memory>
#include <optional>
#include <string>

#include "GuiGroupEdit.hpp"
#include "GuiHelper.hpp"
#include "GuiPlotEdit.hpp"
#include "GuiStatisticsWindow.hpp"
#include "I18n.hpp"
#include "IFileHandler.hpp"
#include "Plot.hpp"
#include "PlotGroupHandler.hpp"
#include "RecorderHandler.hpp"
#include "ViewerDataHandler.hpp"

class PlotsTree
{
   public:
	PlotsTree(ViewerDataHandler* viewerDataHandler, PlotHandler* plotHandler, PlotGroupHandler* plotGroupHandler, VariableHandler* variableHandler, std::shared_ptr<PlotEditWindow> plotEditWindow, IFileHandler* fileHandler, recorder::RecorderHandler* recorderHandler, spdlog::logger* logger) : viewerDataHandler(viewerDataHandler), plotHandler(plotHandler), plotGroupHandler(plotGroupHandler), variableHandler(variableHandler), plotEditWindow(plotEditWindow), fileHandler(fileHandler), recorderHandler(recorderHandler), logger(logger)
	{
		groupEditWindow = std::make_unique<GroupEditWindow>(plotGroupHandler);
	}
	void draw()
	{
		const uint32_t windowHeight = 350 * GuiHelper::contentScale;

		ImGui::Dummy(ImVec2(-1, 5));
		GuiHelper::drawCenteredText(I18n::tr("Plots"));
		ImGui::Separator();

		if (plotHandler->getPlotsCount() == 0)
		{
			selectedGroup = "new group0";
			selectedPlot = "new plot0";
			auto group = plotGroupHandler->addGroup("new group0");
			auto plot = plotHandler->addPlot("new plot0");
			group->addPlot(plot);
		}

		if (!plotHandler->checkIfPlotExists(selectedPlot))
			selectFirstPlotOfActiveGroup();

		if (!plotGroupHandler->checkIfGroupExists(selectedGroup))
			selectedGroup = plotGroupHandler->getActiveGroup()->getName();

		/* The controls column has to be wide enough for a label and two
		   controls to share one line, which is how the rows below are laid
		   out. It is given that width and the tree takes what is left, so the
		   rows stay on one line whatever the dock has been resized to. The
		   tree keeps a floor so it cannot be squeezed away entirely. */
		const float available = ImGui::GetContentRegionAvail().x;
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float panelWidth = 195.0f * GuiHelper::contentScale;
		const float minimumTree = 120.0f * GuiHelper::contentScale;

		float treeWidth = available - panelWidth - spacing;

		if (treeWidth < minimumTree)
			treeWidth = minimumTree;

		ImGui::BeginChild(I18n::tr("Plot Tree"), ImVec2(-1, windowHeight));
		ImGui::BeginChild("left pane", ImVec2(treeWidth, -1), true);

		/* Only the groups without a parent start a branch; the rest are reached
		   by recursing, which is what makes the tree as deep as the user built
		   it rather than one level. */
		for (const std::string& rootName : plotGroupHandler->getRootNames())
			drawGroupNode(rootName);

		/* A right click that misses every row lands on the panel itself. There
		   is still something to offer there: the groups can be made from it, so
		   a tree that has nothing in it is not a dead end. Plots are not on
		   offer, because a plot belongs to a group and this menu is about the
		   panel rather than about any group. The rows keep their own menus:
		   the flag is what sends a click on a row to the row. */
		if (ImGui::BeginPopupContextWindow("##plot tree background", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			const bool recorderReady = recorderHandler != nullptr && recorderHandler->isEnabled();

			if (ImGui::BeginMenu(I18n::tr("New")))
			{
				if (ImGui::MenuItem(I18n::tr("Sampling group")))
					requestNewGroup(PlotGroup::Type::Sampling);

				ImGui::BeginDisabled(!recorderReady);

				if (ImGui::MenuItem(I18n::tr("Recorder group")))
					requestNewGroup(PlotGroup::Type::Recorder);

				ImGui::EndDisabled();

				if (!recorderReady && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip(I18n::tr("Please enable recorder in Acquisition settings to add a new recorder group"));

				ImGui::EndMenu();
			}

			ImGui::EndPopup();
		}

		/* What the menus asked for is applied once the whole tree has been
		   walked: adding or removing a plot changes the map the walk is
		   reading, so it cannot happen in the middle of it. */
		applyPendingAction();

		ImGui::EndChild();
		ImGui::SameLine();

		groupEditWindow->draw();

		std::shared_ptr<Plot> plt = plotHandler->checkIfPlotExists(selectedPlot) ? plotHandler->getPlot(selectedPlot) : nullptr;

		/* A group can be empty, and then there is nothing on the right to show. */
		if (plt == nullptr)
		{
			ImGui::BeginGroup();
			GuiHelper::drawCenteredText(I18n::tr("No plot selected"));
			ImGui::EndGroup();
			ImGui::EndChild();
			return;
		}

		ImGui::BeginGroup();
		ImGui::PushID(plt->getName().c_str());

		/* The cursors are put away while the plot is being filled with
		   samples, because a measurement taken on data that is still
		   arriving is a measurement of nothing. */
		if (viewerDataHandler->getState() == ViewerDataHandler::State::RUN)
		{
			plt->setCursorsVisible(false);
			plt->setStatisticsVisible(false);
			plt->statisticsSeries = 0;
		}

		/* The cursors row stands above every plot type, the same way it does
		   in the reference build: which lines a plot can carry is not
		   decided by how it draws its data. A plot whose horizontal axis is
		   another variable gets its slope reading out of the pair of
		   vertical cursors, so the row is not limited to the time plots. */
		drawCursorSettings(plt);
		drawStatisticsSettings(plt);
		statisticsWindow.drawAnalog(plt);
		ImGui::PopID();

		/* Var list within plot*/
		ImGui::PushID("list");
		if (ImGui::BeginListBox("##", ImVec2(-1, windowHeight - 100 * GuiHelper::contentScale)))
		{
			std::optional<std::string> seriesNameToDelete = {};
			for (auto& [name, ser] : plt->getSeriesMap())
			{
				ImGui::BeginDisabled(!ser->var->getIsCurrentlySampled() && viewerDataHandler->getState() == DataHandlerBase::State::RUN);
				ImGui::PushID(name.c_str());
				ImGui::Checkbox("", &ser->visible);
				ImGui::PopID();
				ImGui::SameLine();
				ImGui::PushID(name.c_str());
				ImGui::ColorEdit4("##", &ser->var->getColor().r, ImGuiColorEditFlags_NoInputs);
				ImGui::SameLine();
				ImGui::Selectable(name.c_str());
				if (!seriesNameToDelete.has_value())
					seriesNameToDelete = GuiHelper::showDeletePopup(I18n::tr("Delete var"), name);
				ImGui::PopID();
				ImGui::EndDisabled();
			}
			plt->removeSeries(seriesNameToDelete.value_or(""));

			ImGui::EndListBox();
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MY_DND"))
			{
				std::set<std::string>* selection = *(std::set<std::string>**)payload->Data;

				for (const auto& name : *selection)
					plt->addSeries(variableHandler->getVariable(name).get());
				selection->clear();
			}
			ImGui::EndDragDropTarget();
		}
		drawExportPlotToCSVButton(plt);
		ImGui::PopID();
		ImGui::EndGroup();
		ImGui::EndChild();
	}

	/* Everything below is the tree's own business: the panel draws itself, and
	   the groups and plots are made, copied and removed from here. */

   private:
	/* The cursors panel and the statistics row.

	   Each pairs a label with its controls on one line, which is how the same
	   panel reads upstream. The label column is measured in characters rather
	   than pixels because that is what the helper pads with, and both rows
	   share one so that their switches stand in the same place. */
	static constexpr size_t alignment = 11;

	void drawCursorSettings(const std::shared_ptr<Plot>& plt)
	{
		GuiHelper::drawTextAlignedToSize(I18n::tr("Cursors:"), alignment);
		ImGui::SameLine();

		bool visible = plt->getCursorsVisible();
		if (ImGui::Checkbox("##cursors", &visible))
		{
			plt->setCursorsVisible(visible);

			/* Switching them back on places them again rather than leaving
			   them wherever they were left, which would be a position from a
			   run the user has since moved on from. */
			if (visible)
				CursorRenderer::resetCursors(plt.get());
		}

		/* The direction only has a meaning once there are cursors to point
		   it at, so it is drawn only then. The reference build does the
		   same, and it keeps the row down to one control until the user has
		   asked for something that needs a second.

		   The same-line call belongs inside the branch along with the combo.
		   Left standing on its own, it waits for an item that the branch may
		   never draw, and then the next row - the statistics switch - is
		   pulled up onto the switch's line. */
		if (visible)
		{
			ImGui::SameLine();

			int32_t mode = static_cast<int32_t>(Plot::cursorModeToIndex(plt->getCursorMode()));
			ImGui::SetNextItemWidth(70 * GuiHelper::contentScale);
			if (ImGui::Combo("##cursorMode", &mode, Plot::cursorModeNames, IM_ARRAYSIZE(Plot::cursorModeNames)))
				plt->setCursorMode(Plot::cursorModeFromIndex(static_cast<uint32_t>(mode)));
		}
	}

	void drawStatisticsSettings(const std::shared_ptr<Plot>& plt)
	{
		GuiHelper::drawTextAlignedToSize(I18n::tr("Statistics:"), alignment);
		ImGui::SameLine();

		bool visible = plt->getStatisticsVisible();
		if (ImGui::Checkbox("##statistics", &visible))
			plt->setStatisticsVisible(visible);
	}

	/* A new plot lands in the group the menu was opened on, takes the kind the
	   entry named, and opens the editor on itself: a plot that has just been
	   made holds no variables yet, so there is nothing to look at until one is
	   added. The name follows the reference build - the same stem with the
	   first free number after it. */
	std::shared_ptr<Plot> createPlot(const std::string& groupName, Plot::Type type)
	{
		const std::shared_ptr<PlotGroup> group = resolveGroup(groupName);
		const std::shared_ptr<Plot> plot = plotHandler->addPlot(plotHandler->makeFreeName("new plot"));

		plot->setType(type);
		group->addPlot(plot);

		logger->info("Adding plot: {} ({})", plot->getName(), plotTypeName(type));

		selectedGroup = group->getName();
		selectedPlot = plot->getName();

		plotEditWindow->setPlotToEdit(plot);
		plotEditWindow->setShowPlotEditWindowState(true);

		return plot;
	}

	/* A new group is made the kind the entry named and carries that kind in its
	   name, so the two kinds stay apart at a glance. It goes to the top level,
	   which is where the reference build puts one, and the editor opens on it
	   so that it can be renamed or nested straight away. */
	std::shared_ptr<PlotGroup> createGroup(PlotGroup::Type type)
	{
		const std::string stem = type == PlotGroup::Type::Recorder ? "new recorder group" : "new sampling group";
		const std::shared_ptr<PlotGroup> group = plotGroupHandler->addGroup(plotGroupHandler->makeFreeName(stem), type);

		logger->info("Adding group: {} ({})", group->getName(), group->getTypeName());

		selectedGroup = group->getName();

		groupEditWindow->setGroupToEdit(group);
		groupEditWindow->setShowGroupEditWindowState(true);

		return group;
	}

	/* A copy holds everything the original holds: its plots, and the groups
	   nested in it. The kind is asked for because the same call answers both
	   "copy this group" and "copy it to the other kind of group". */
	void copyGroup(const std::string& name, PlotGroup::Type type)
	{
		const std::string copyName = plotGroupHandler->copyGroup(name, type, *plotHandler);

		if (copyName.empty())
			return;

		logger->info("Copied group {} as {}", name, copyName);

		selectedGroup = copyName;
	}

	/* A copy of one plot lands in the group it was copied from, with the new
	   samples it collects from here on. */
	void copyPlot(const std::string& groupName, const std::string& plotName)
	{
		if (!plotHandler->checkIfPlotExists(plotName))
			return;

		const std::shared_ptr<PlotGroup> group = resolveGroup(groupName);
		const std::shared_ptr<Plot> copy = plotHandler->addPlotCopy(*plotHandler->getPlot(plotName), plotName);

		group->addPlot(copy);

		logger->info("Copied plot {} as {}", plotName, copy->getName());

		selectedGroup = group->getName();
		selectedPlot = copy->getName();
	}

	/* The group a menu was opened on, or the active one when the name no longer
	   names anything - a group can be removed while its menu is still open. */
	std::shared_ptr<PlotGroup> resolveGroup(const std::string& groupName)
	{
		return plotGroupHandler->checkIfGroupExists(groupName) ? plotGroupHandler->getGroup(groupName) : plotGroupHandler->getActiveGroup();
	}

	/* What a context menu asked for. The action is taken once the tree has been
	   walked, because adding or removing a plot or a group changes the map the
	   walk is reading. */
	enum class PendingKind : uint8_t
	{
		None = 0,
		NewPlot,
		NewGroup,
		CopyPlot,
		CopyGroup,
		DeletePlot,
		DeleteGroup,
	};

	void requestNewPlot(const std::string& groupName, Plot::Type type)
	{
		pending = PendingKind::NewPlot;
		pendingGroup = groupName;
		pendingPlotType = type;
	}

	void requestNewGroup(PlotGroup::Type type)
	{
		pending = PendingKind::NewGroup;
		pendingGroupType = type;
	}

	void requestCopyPlot(const std::string& groupName, const std::string& plotName)
	{
		pending = PendingKind::CopyPlot;
		pendingGroup = groupName;
		pendingPlot = plotName;
	}

	void requestCopyGroup(const std::string& groupName, PlotGroup::Type type)
	{
		pending = PendingKind::CopyGroup;
		pendingGroup = groupName;
		pendingGroupType = type;
	}

	void requestDeletePlot(const std::string& groupName, const std::string& plotName)
	{
		pending = PendingKind::DeletePlot;
		pendingGroup = groupName;
		pendingPlot = plotName;
	}

	void requestDeleteGroup(const std::string& groupName)
	{
		pending = PendingKind::DeleteGroup;
		pendingGroup = groupName;
	}

	void applyPendingAction()
	{
		const PendingKind action = pending;

		pending = PendingKind::None;

		switch (action)
		{
			case PendingKind::NewPlot:
				createPlot(pendingGroup, pendingPlotType);
				break;

			case PendingKind::NewGroup:
				createGroup(pendingGroupType);
				break;

			case PendingKind::CopyPlot:
				copyPlot(pendingGroup, pendingPlot);
				break;

			case PendingKind::CopyGroup:
				copyGroup(pendingGroup, pendingGroupType);
				break;

			case PendingKind::DeletePlot:
				if (plotGroupHandler->checkIfGroupExists(pendingGroup))
					plotGroupHandler->getGroup(pendingGroup)->removePlot(pendingPlot);
				break;

			case PendingKind::DeleteGroup:
				plotGroupHandler->removeGroup(pendingGroup);
				break;

			case PendingKind::None:
				break;
		}
	}

	/* The names the plot kinds are offered under in the menu. They line up with
	   Plot::Type, which is what lets the entry be turned back into a kind. */
	static constexpr const char* plotTypeNames[] = {"Curve", "Bar", "Table", "XY"};

	static const char* plotTypeName(Plot::Type type)
	{
		const uint32_t index = static_cast<uint32_t>(type);
		return index < IM_ARRAYSIZE(plotTypeNames) ? plotTypeNames[index] : "Curve";
	}

	/* Draws one group together with its plots and everything nested in it. The
	   parameter names the group, so the same group object cannot be drawn twice
	   even if two branches of a hand-edited file referred to it. */
	void drawGroupNode(const std::string& name)
	{
		if (!plotGroupHandler->checkIfGroupExists(name))
			return;

		const std::shared_ptr<PlotGroup> group = plotGroupHandler->getGroup(name);

		ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_OpenOnArrow;

		if (selectedGroup == name)
		{
			nodeFlags |= ImGuiTreeNodeFlags_Selected;
			plotGroupHandler->setActiveGroup(name);
		}

		/* A group that holds other groups opens by default, so a project that was
		   just loaded shows its whole shape instead of one collapsed line. */
		if (plotGroupHandler->hasChildren(name))
			nodeFlags |= ImGuiTreeNodeFlags_DefaultOpen;

		const bool state = ImGui::TreeNodeEx(group->getName().c_str(), nodeFlags);

		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
			selectedGroup = name;

		drawMenuGroupPopup(name);

		if (state)
		{
			/* Drag n Drop target for plots within groups */
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PLOT"))
				{
					std::string dropped = *(std::string*)payload->Data;
					group->addPlot(plotHandler->getPlot(dropped));
				}
				ImGui::EndDragDropTarget();
			}

			for (auto& [plotId, plotElem] : *group)
			{
				auto plot = plotElem.plot;
				ImGui::PushID("plot");

				ImGui::Checkbox(std::string("##" + plotId).c_str(), (bool*)&plotElem.visibility);
				ImGui::SameLine();

				bool shouldSelect = (selectedPlot == plotId && plotGroupHandler->getActiveGroup() == group);

				if (ImGui::Selectable(plotId.c_str(), shouldSelect, ImGuiSelectableFlags_AllowDoubleClick))
				{
					selectedPlot = plotId;

					if (ImGui::IsMouseDoubleClicked(0))
					{
						plotEditWindow->setPlotToEdit(plot);
						plotEditWindow->setShowPlotEditWindowState(true);
					}
				}

				drawMenuPlotPopup(name, plotId, plot);

				/* Drag n Drop source for plots within groups */
				if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
				{
					ImGui::SetDragDropPayload("PLOT", &plotId, sizeof(plotId));
					ImGui::TextUnformatted(plotId.c_str());
					ImGui::EndDragDropSource();
				}

				if (plot->isHovered() && ImGui::IsMouseClicked(0))
					selectedPlot = plot->getName();

				ImGui::PopID();
			}

			/* Children are drawn inside the expanded node, which is what puts
			   them one level deeper than their parent. */
			for (const std::string& childName : plotGroupHandler->getChildNames(name))
				drawGroupNode(childName);

			ImGui::TreePop();
		}
	}

	/* The panel on the right shows one plot. When the remembered name is gone -
	   the plot was deleted, or the group it belonged to went away - the first
	   plot of the active group takes its place. */
	void selectFirstPlotOfActiveGroup()
	{
		selectedPlot = "";

		const std::shared_ptr<PlotGroup> group = plotGroupHandler->getActiveGroup();

		if (group == nullptr)
			return;

		auto first = group->begin();

		if (first != group->end() && first->second.plot != nullptr)
			selectedPlot = first->second.plot->getName();
	}

	void drawExportPlotToCSVButton(std::shared_ptr<Plot> plt)
	{
		if (ImGui::Button(I18n::tr("Export plot to *.csv"), ImVec2(-1, 25 * GuiHelper::contentScale)))
		{
			std::string path = fileHandler->saveFile({{"CSV", "csv"}});
			std::ofstream csvFile(path);

			if (!csvFile)
			{
				logger->info("Error opening the file: {}", path);
				return;
			}

			uint32_t dataSize = plt->getXAxisSeries()->getSize();

			csvFile << "time [s],";

			for (auto& [name, ser] : plt->getSeriesMap())
				csvFile << name << ",";

			csvFile << std::endl;

			for (size_t i = 0; i < dataSize; ++i)
			{
				uint32_t offset = plt->getXAxisSeries()->getOffset();
				uint32_t index = (offset + i < dataSize) ? offset + i : i - (dataSize - offset);
				csvFile << plt->getXAxisSeries()->getFirstElementCopy()[index] << ",";

				for (auto& [name, ser] : plt->getSeriesMap())
					csvFile << ser->buffer->getFirstElementCopy()[index] << ",";

				csvFile << std::endl;
			}

			csvFile.close();
		}
	}

	/* The menu a group node answers a right click with.
	   It is where everything about a group is done: making one of either kind,
	   making a plot of a named kind in it, copying it, and copying it to the
	   other kind. Where the reference build offers these, this menu offers
	   them too, under the same wording, so a reader of its manual finds them
	   where they expect. The separators are part of that: the reference build
	   breaks the entries into making, copying, deleting and properties, and a
	   menu that runs them together reads as a different menu.

	   The entries that would produce a recorder group are greyed out while the
	   recorder is switched off, and they say why through a tooltip rather than
	   leaving the user to guess - a greyed entry with no reason is a bug from
	   where the user is sitting. */
	void drawMenuGroupPopup(const std::string& name)
	{
		const std::shared_ptr<PlotGroup> group = plotGroupHandler->getGroup(name);

		if (group == nullptr)
			return;

		const bool isRecorderGroup = group->getType() == PlotGroup::Type::Recorder;
		const bool recorderReady = recorderHandler != nullptr && recorderHandler->isEnabled();

		ImGui::PushID(name.c_str());

		if (ImGui::BeginPopupContextItem(name.c_str()))
		{
			if (ImGui::BeginMenu(I18n::tr("New")))
			{
				if (ImGui::MenuItem(I18n::tr("Sampling group")))
					requestNewGroup(PlotGroup::Type::Sampling);

				ImGui::BeginDisabled(!recorderReady);

				if (ImGui::MenuItem(I18n::tr("Recorder group")))
					requestNewGroup(PlotGroup::Type::Recorder);

				ImGui::EndDisabled();

				if (!recorderReady && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip(I18n::tr("Please enable recorder in Acquisition settings to add a new recorder group"));

				if (ImGui::BeginMenu(I18n::tr("Plot")))
				{
					for (uint32_t index = 0; index < IM_ARRAYSIZE(plotTypeNames); index++)
					{
						if (ImGui::MenuItem(I18n::tr(plotTypeNames[index])))
							requestNewPlot(name, static_cast<Plot::Type>(index));
					}

					ImGui::EndMenu();
				}

				ImGui::EndMenu();
			}

			ImGui::Separator();

			if (ImGui::MenuItem(I18n::tr("Copy")))
				requestCopyGroup(name, group->getType());

			/* A group is copied to the other kind of group. Which kind that is
			   follows from the group the menu was opened on, so only one of the
			   two entries is ever offered. */
			if (isRecorderGroup)
			{
				if (ImGui::MenuItem(I18n::tr("Copy to sampling group")))
					requestCopyGroup(name, PlotGroup::Type::Sampling);
			}
			else
			{
				ImGui::BeginDisabled(!recorderReady);

				if (ImGui::MenuItem(I18n::tr("Copy to recorder group")))
					requestCopyGroup(name, PlotGroup::Type::Recorder);

				ImGui::EndDisabled();

				if (!recorderReady && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip(I18n::tr("Please enable recorder in Acquisition settings to copy this group to a recorder group"));
			}

			ImGui::Separator();

			if (ImGui::MenuItem(I18n::tr("Delete group")))
				requestDeleteGroup(name);

			ImGui::Separator();

			if (ImGui::MenuItem(I18n::tr("Properties")))
			{
				groupEditWindow->setGroupToEdit(group);
				groupEditWindow->setShowGroupEditWindowState(true);
			}

			ImGui::EndPopup();
		}

		ImGui::PopID();
	}

	/* The menu a plot row answers a right click with. A plot is made with a
	   kind from the start here as well, and a plot can be copied the same way a
	   group is, which is what "one more of these" usually means. */
	void drawMenuPlotPopup(const std::string& groupName, const std::string& plotName, const std::shared_ptr<Plot>& plot)
	{
		ImGui::PushID(plotName.c_str());

		if (ImGui::BeginPopupContextItem(plotName.c_str()))
		{
			if (ImGui::BeginMenu(I18n::tr("New")))
			{
				if (ImGui::BeginMenu(I18n::tr("Plot")))
				{
					for (uint32_t index = 0; index < IM_ARRAYSIZE(plotTypeNames); index++)
					{
						if (ImGui::MenuItem(I18n::tr(plotTypeNames[index])))
							requestNewPlot(groupName, static_cast<Plot::Type>(index));
					}

					ImGui::EndMenu();
				}

				ImGui::EndMenu();
			}

			ImGui::Separator();

			if (ImGui::MenuItem(I18n::tr("Copy")))
				requestCopyPlot(groupName, plotName);

			ImGui::Separator();

			if (ImGui::MenuItem(I18n::tr("Delete plot")))
				requestDeletePlot(groupName, plotName);

			ImGui::Separator();

			if (ImGui::MenuItem(I18n::tr("Properties")))
			{
				plotEditWindow->setPlotToEdit(plot);
				plotEditWindow->setShowPlotEditWindowState(true);
			}

			ImGui::EndPopup();
		}

		ImGui::PopID();
	}

   private:
	ViewerDataHandler* viewerDataHandler;
	PlotHandler* plotHandler;
	PlotGroupHandler* plotGroupHandler;
	VariableHandler* variableHandler;
	std::shared_ptr<PlotEditWindow> plotEditWindow;

	/* Which group and plot the tree has selected. Held here rather than as
	   locals of draw(), because the branch that draws a nested group is a
	   method of its own and has to read and write the same selection. */
	std::string selectedGroup = "";
	std::string selectedPlot = "";

	/* What a context menu asked for, and what it was asked about. */
	PendingKind pending = PendingKind::None;
	std::string pendingGroup = "";
	std::string pendingPlot = "";
	Plot::Type pendingPlotType = Plot::Type::CURVE;
	PlotGroup::Type pendingGroupType = PlotGroup::Type::Sampling;

	std::unique_ptr<GroupEditWindow> groupEditWindow;

	StatisticsWindow statisticsWindow;
	IFileHandler* fileHandler;
	recorder::RecorderHandler* recorderHandler;
	spdlog::logger* logger;
};