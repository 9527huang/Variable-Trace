#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "GuiHelper.hpp"
#include "Popup.hpp"
#include "VariableHandler.hpp"
#include "WritePlanner.hpp"
#include "imgui.h"
#include "implot.h"

/*
 * The window that edits the write plans.
 *
 * Left pane: the list of plans, with a remove button per entry.
 * Right pane: the plan that is selected, as a name, the variable it writes and
 * a table of (time, value) steps, with its curve drawn underneath.
 *
 * The curve is drawn from the same steps the table shows, so a step that is
 * typed here and a step that is drawn below cannot disagree.
 */
class WritePlannerWindow
{
   public:
	WritePlannerWindow(WritePlanner* planner, VariableHandler* variableHandler) : planner(planner), variableHandler(variableHandler)
	{
	}

	void draw()
	{
		if (showWindow)
			ImGui::OpenPopup("Write Planner");

		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(900 * GuiHelper::contentScale, 560 * GuiHelper::contentScale));

		if (!ImGui::BeginPopupModal("Write Planner", &showWindow, 0))
			return;

		ImGui::BeginChild("##WPContent", ImVec2(-1, -1));
		drawPlanList();
		ImGui::SameLine();
		drawPlanDetails();
		ImGui::EndChild();

		popup.handle();
		ImGui::EndPopup();
	}

	void setShowWindowState(bool state)
	{
		showWindow = state;
	}

	bool getShowWindowState() const
	{
		return showWindow;
	}

   private:
	void drawPlanList()
	{
		ImGui::BeginChild("##PlanList", ImVec2(230 * GuiHelper::contentScale, -1), true);

		GuiHelper::drawCenteredText("Plans");
		ImGui::Separator();

		if (ImGui::Button("Add plan", ImVec2(-1, 25 * GuiHelper::contentScale)))
		{
			const std::string name = WritePlanner::freeName(*planner, "plan");
			planner->addPlan(name);
			selectedPlan = name;
		}

		std::optional<std::string> nameToRemove;

		ImGui::BeginChild("##PlanListItems", ImVec2(-1, -1));

		for (const std::string& name : planner->getNames())
		{
			ImGui::PushID(name.c_str());

			/* The remove button goes first so the name keeps the rest of the row;
			   a selectable that spans the row would swallow the click meant for
			   the button. */
			if (ImGui::SmallButton("x"))
				nameToRemove = name;

			ImGui::SameLine();

			if (ImGui::Selectable(name.c_str(), name == selectedPlan, 0, ImVec2(-1, 0)))
				selectedPlan = name;

			ImGui::PopID();
		}

		ImGui::EndChild();

		if (nameToRemove.has_value())
		{
			planner->removePlan(*nameToRemove);

			if (selectedPlan == *nameToRemove)
			{
				const std::vector<std::string> remaining = planner->getNames();
				selectedPlan = remaining.empty() ? "" : remaining.front();
			}
		}

		ImGui::EndChild();
	}

	void drawPlanDetails()
	{
		ImGui::BeginChild("##PlanDetails", ImVec2(-1, -1));

		if (selectedPlan.empty() || !planner->hasPlan(selectedPlan))
		{
			ImGui::Dummy(ImVec2(-1, 20));
			GuiHelper::drawCenteredText("No plan selected");
			ImGui::Dummy(ImVec2(-1, 5));
			GuiHelper::drawCenteredText("Select a plan to edit");
			ImGui::EndChild();
			return;
		}

		const WritePlanner::Plan plan = planner->getPlan(selectedPlan);

		/* NAME */
		std::string name = plan.name;

		GuiHelper::drawTextAlignedToSize("name:", alignment);
		ImGui::SameLine();
		ImGui::InputText("##name", &name, ImGuiInputTextFlags_None, NULL, NULL);

		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			if (name != plan.name && !planner->renamePlan(plan.name, name))
				popup.show("Error!", "A plan with that name already exists!", 1.5f);
			else
				selectedPlan = name;
		}

		/* VARIABLE */
		std::string variable = plan.variable;

		GuiHelper::drawTextAlignedToSize("variable:", alignment);
		ImGui::SameLine();

		if (ImGui::InputText("##variable", &variable, ImGuiInputTextFlags_None, NULL, NULL))
			planner->setVariable(plan.name, variable);

		ImGui::SameLine();

		/* A name the variable table does not know would write nowhere, so it is
		   pointed out as it is typed rather than at the moment of use. */
		if (variable.empty() || variableHandler == nullptr || variableHandler->contains(variable))
			ImGui::TextDisabled("(the variable this plan is for)");
		else
			ImGui::TextColored(GuiHelper::orange, "variable does not exist!");

		drawSteps(plan);
		drawPreview(plan);

		ImGui::EndChild();
	}

	void drawSteps(const WritePlanner::Plan& plan)
	{
		ImGui::Dummy(ImVec2(-1, 5));
		GuiHelper::drawCenteredText("Steps");
		ImGui::Separator();

		/* ADD */
		ImGui::SetNextItemWidth(90 * GuiHelper::contentScale);
		ImGui::InputText("Time##add", &newStepTime, ImGuiInputTextFlags_CharsDecimal, NULL, NULL);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(90 * GuiHelper::contentScale);
		ImGui::InputText("Value##add", &newStepValue, ImGuiInputTextFlags_CharsDecimal, NULL, NULL);
		ImGui::SameLine();

		if (ImGui::Button("Add", ImVec2(80 * GuiHelper::contentScale, 0)))
		{
			const double time = GuiHelper::convertStringToNumber<double>(newStepTime);
			planner->addStep(plan.name, time, GuiHelper::convertStringToNumber<double>(newStepValue));

			/* The next step defaults to a moment after the one just added, which
			   is what building a ramp by hand needs. */
			newStepTime = GuiHelper::numberToString(time + 1.0);
		}

		ImGui::SameLine();
		ImGui::HelpMarker("A step holds its value until the next step begins.");

		/* TABLE */
		std::vector<WritePlanner::Step> steps = plan.steps;
		std::optional<size_t> indexToRemove;

		if (ImGui::BeginTable("##StepsTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 140 * GuiHelper::contentScale)))
		{
			ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 50 * GuiHelper::contentScale);
			ImGui::TableHeadersRow();

			for (size_t index = 0; index < steps.size(); index++)
			{
				ImGui::TableNextRow();
				ImGui::PushID(static_cast<int>(index));

				ImGui::TableSetColumnIndex(0);
				std::string time = GuiHelper::numberToString(steps[index].time);

				if (ImGui::InputText("##time", &time, ImGuiInputTextFlags_CharsDecimal, NULL, NULL))
					steps[index].time = GuiHelper::convertStringToNumber<double>(time);

				ImGui::TableSetColumnIndex(1);
				std::string value = GuiHelper::numberToString(steps[index].value);

				if (ImGui::InputText("##value", &value, ImGuiInputTextFlags_CharsDecimal, NULL, NULL))
					steps[index].value = GuiHelper::convertStringToNumber<double>(value);

				ImGui::TableSetColumnIndex(2);

				if (ImGui::Button("Del", ImVec2(-1, 0)))
					indexToRemove = index;

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		/* A row that was deleted is deleted through the model, because the local
		   copy is thrown away at the end of the frame anyway. */
		if (indexToRemove.has_value())
			planner->removeStep(plan.name, *indexToRemove);
		else if (steps != plan.steps)
			planner->setSteps(plan.name, steps);
	}

	void drawPreview(const WritePlanner::Plan& plan)
	{
		ImGui::Dummy(ImVec2(-1, 5));

		const double length = WritePlanner::duration(plan);
		const double span = length > 0.0 ? length * 1.1 : 1.0;

		/* The curve is built from the steps rather than read from the target, so
		   the picture is the plan and not the measurement. */
		const size_t sampleCount = 256;
		std::vector<double> times;
		std::vector<double> values;
		times.reserve(sampleCount);
		values.reserve(sampleCount);

		for (size_t index = 0; index < sampleCount; index++)
		{
			const double time = span * static_cast<double>(index) / static_cast<double>(sampleCount - 1);
			times.push_back(time);
			values.push_back(WritePlanner::valueAt(plan, time));
		}

		double lowest = values.front();
		double highest = values.front();

		for (const double value : values)
		{
			lowest = std::min(lowest, value);
			highest = std::max(highest, value);
		}

		/* A flat plan would leave a zero height axis, which ImPlot draws as a
		   line at the border. */
		if (highest - lowest < 1e-9)
		{
			lowest -= 0.5;
			highest += 0.5;
		}

		if (ImPlot::BeginPlot("##WritePlanPlot", ImVec2(-1, 200 * GuiHelper::contentScale)))
		{
			ImPlot::SetupAxes("time [s]", "value");
			ImPlot::SetupAxesLimits(0.0, span, lowest - (highest - lowest) * 0.05, highest + (highest - lowest) * 0.05, ImPlotCond_Once);
			ImPlot::PlotLine(plan.name.c_str(), times.data(), values.data(), static_cast<int>(times.size()));
			ImPlot::EndPlot();
		}
	}

   private:
	/* Text alignment in front of the labelled input fields, as in the other
	   edit windows. */
	static constexpr size_t alignment = 12;

	WritePlanner* planner;
	VariableHandler* variableHandler;

	std::string selectedPlan = "";

	/* What the add row currently holds. Kept here rather than as a local so that
	   a value typed but not yet added survives the frame. */
	std::string newStepTime = "0";
	std::string newStepValue = "0";

	bool showWindow = false;

	Popup popup;
};
