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

		ImGui::BeginChild("Plot Tree", ImVec2(-1, windowHeight));
		ImGui::BeginChild("left pane", ImVec2(200 * GuiHelper::contentScale, -1), true);

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

		/* reset markers when viewer is running */
		if (viewerDataHandler->getState() == ViewerDataHandler::State::RUN)
		{
			plt->markerX0.setState(false);
			plt->markerX1.setState(false);
			plt->statisticsSeries = 0;
		}

		/* Staticstics */
		ImGui::BeginDisabled(plt->getType() != Plot::Type::CURVE);
		bool mx0 = plt->markerX0.getState();
		bool mx1 = plt->markerX1.getState();
		ImGui::Text("x0 marker  ");
		ImGui::SameLine();
		ImGui::Checkbox("##mx0", &mx0);
		plt->markerX0.setState(mx0);
		ImGui::Text("x1 marker  ");
		ImGui::SameLine();
		ImGui::Checkbox("##mx1", &mx1);
		plt->markerX1.setState(mx1);
		statisticsWindow.drawAnalog(plt);
		ImGui::EndDisabled();
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