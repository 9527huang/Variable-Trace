#pragma once

#include <memory>
#include <optional>
#include <string>

#include "GuiGroupEdit.hpp"
#include "GuiHelper.hpp"
#include "GuiPlotEdit.hpp"
#include "GuiStatisticsWindow.hpp"
#include "IFileHandler.hpp"
#include "Plot.hpp"
#include "PlotGroupHandler.hpp"
#include "ViewerDataHandler.hpp"

class PlotsTree
{
   public:
	PlotsTree(ViewerDataHandler* viewerDataHandler, PlotHandler* plotHandler, PlotGroupHandler* plotGroupHandler, VariableHandler* variableHandler, std::shared_ptr<PlotEditWindow> plotEditWindow, IFileHandler* fileHandler, spdlog::logger* logger) : viewerDataHandler(viewerDataHandler), plotHandler(plotHandler), plotGroupHandler(plotGroupHandler), variableHandler(variableHandler), plotEditWindow(plotEditWindow), fileHandler(fileHandler), logger(logger)
	{
		groupEditWindow = std::make_unique<GroupEditWindow>(plotGroupHandler);
	}
	void draw()
	{
		const uint32_t windowHeight = 350 * GuiHelper::contentScale;

		ImGui::Dummy(ImVec2(-1, 5));
		GuiHelper::drawCenteredText("Plots");
		ImGui::Separator();

		drawAddPlotButton();

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

		ImGui::BeginChild("Plot Tree", ImVec2(-1, windowHeight));
		ImGui::BeginChild("left pane", ImVec2(treeWidth, -1), true);

		/* Only the groups without a parent start a branch; the rest are reached
		   by recursing, which is what makes the tree as deep as the user built
		   it rather than one level. */
		std::optional<std::string> groupNameToDelete;

		for (const std::string& rootName : plotGroupHandler->getRootNames())
			drawGroupNode(rootName, groupNameToDelete);

		if (groupNameToDelete.has_value())
			plotGroupHandler->removeGroup(groupNameToDelete.value());

		ImGui::EndChild();
		ImGui::SameLine();

		groupEditWindow->draw();

		std::shared_ptr<Plot> plt = plotHandler->checkIfPlotExists(selectedPlot) ? plotHandler->getPlot(selectedPlot) : nullptr;

		/* A group can be empty, and then there is nothing on the right to show. */
		if (plt == nullptr)
		{
			ImGui::BeginGroup();
			GuiHelper::drawCenteredText("No plot selected");
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
					seriesNameToDelete = GuiHelper::showDeletePopup("Delete var", name);
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

	/* The cursors panel and the statistics row.

	   Each pairs a label with its controls on one line, which is how the same
	   panel reads upstream. The label column is measured in characters rather
	   than pixels because that is what the helper pads with, and both rows
	   share one so that their switches stand in the same place. */
	static constexpr size_t alignment = 11;

	void drawCursorSettings(const std::shared_ptr<Plot>& plt)
	{
		GuiHelper::drawTextAlignedToSize("Cursors:", alignment);
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
		GuiHelper::drawTextAlignedToSize("Statistics:", alignment);
		ImGui::SameLine();

		bool visible = plt->getStatisticsVisible();
		if (ImGui::Checkbox("##statistics", &visible))
			plt->setStatisticsVisible(visible);
	}

	void drawAddPlotButton()
	{
		if (ImGui::Button("Add plot", ImVec2(-1, 25 * GuiHelper::contentScale)))
			addNewPlot();

		if (ImGui::Button("Add group", ImVec2(-1, 25 * GuiHelper::contentScale)))
			addNewGroup();
	}

	/* Draws one group together with its plots and everything nested in it. The
	   parameter names the group, so the same group object cannot be drawn twice
	   even if two branches of a hand-edited file referred to it. */
	void drawGroupNode(const std::string& name, std::optional<std::string>& groupNameToDelete)
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

		drawMenuGroupPopup(name, [&]()
						   { addNewGroup(name); }, [&]()
						   { addNewPlot(name); }, [&](std::string groupToDelete)
						   { groupNameToDelete = groupToDelete; }, [&](std::string groupToEdit)
						   {
							   groupEditWindow->setGroupToEdit(plotGroupHandler->getGroup(groupToEdit));
							   groupEditWindow->setShowGroupEditWindowState(true); });

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

			std::optional<std::string> plotNameToDelete;

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

				drawMenuPlotPopup(plotId, [&]()
								  { addNewPlot(name); }, [&](std::string plotToDelete)
								  { plotNameToDelete = plotToDelete; }, [&](std::string)
								  {plotEditWindow->setPlotToEdit(plot);
						           plotEditWindow->setShowPlotEditWindowState(true); });

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

			if (plotNameToDelete.has_value())
				group->removePlot(plotNameToDelete.value_or(""));

			/* Children are drawn inside the expanded node, which is what puts
			   them one level deeper than their parent. */
			for (const std::string& childName : plotGroupHandler->getChildNames(name))
				drawGroupNode(childName, groupNameToDelete);

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

	/* An empty group name means the active group, which is what the buttons at
	   the top of the tree want; the context menu passes the group it was opened
	   on so that a new plot lands where the user clicked. */
	void addNewPlot(const std::string& groupName = "")
	{
		uint32_t num = 0;
		while (plotHandler->checkIfPlotExists(std::string("new plot") + std::to_string(num)))
			num++;

		std::string newName = std::string("new plot") + std::to_string(num);
		auto plot = plotHandler->addPlot(newName);

		std::shared_ptr<PlotGroup> target = plotGroupHandler->getActiveGroup();

		if (!groupName.empty() && plotGroupHandler->checkIfGroupExists(groupName))
			target = plotGroupHandler->getGroup(groupName);

		if (target != nullptr)
			target->addPlot(plot);

		plotEditWindow->setPlotToEdit(plot);
		plotEditWindow->setShowPlotEditWindowState(true);
	}

	/* An empty parent name puts the group at the top level. */
	void addNewGroup(const std::string& parentName = "")
	{
		uint32_t num = 0;
		while (plotGroupHandler->checkIfGroupExists(std::string("new group") + std::to_string(num)))
			num++;

		std::string newName = std::string("new group") + std::to_string(num);
		auto group = plotGroupHandler->addGroup(newName, PlotGroup::Type::Sampling, parentName);
		groupEditWindow->setGroupToEdit(group);
		groupEditWindow->setShowGroupEditWindowState(true);
	}

	void drawExportPlotToCSVButton(std::shared_ptr<Plot> plt)
	{
		if (ImGui::Button("Export plot to *.csv", ImVec2(-1, 25 * GuiHelper::contentScale)))
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

   private:
	void drawMenuGroupPopup(const std::string& name, std::function<void()> onNewGroup, std::function<void()> onNewPlot, std::function<void(const std::string&)> onDelete, std::function<void(const std::string&)> onProperties)
	{
		ImGui::PushID(name.c_str());
		if (ImGui::BeginPopupContextItem(name.c_str()))
		{
			if (ImGui::BeginMenu("New"))
			{
				if (ImGui::MenuItem("Group"))
					onNewGroup();

				if (ImGui::MenuItem("Plot"))
					onNewPlot();

				ImGui::EndMenu();
			}

			if (ImGui::MenuItem("Delete group"))
				onDelete(name);

			if (ImGui::MenuItem("Properties"))
				onProperties(name);

			ImGui::EndPopup();
		}
		ImGui::PopID();
	}

	void drawMenuPlotPopup(const std::string& name, std::function<void()> onNewPlot, std::function<void(const std::string&)> onDelete, std::function<void(const std::string&)> onProperties)
	{
		ImGui::PushID(name.c_str());
		if (ImGui::BeginPopupContextItem(name.c_str()))
		{
			if (ImGui::BeginMenu("New"))
			{
				if (ImGui::MenuItem("Plot"))
					onNewPlot();

				ImGui::EndMenu();
			}

			if (ImGui::MenuItem("Delete plot"))
				onDelete(name);

			if (ImGui::MenuItem("Properties"))
				onProperties(name);

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

	std::unique_ptr<GroupEditWindow> groupEditWindow;

	StatisticsWindow statisticsWindow;
	IFileHandler* fileHandler;
	spdlog::logger* logger;
};