#ifndef _GUI_PLOTEDIT_HPP
#define _GUI_PLOTEDIT_HPP

#include "GuiHelper.hpp"
#include "GuiSelectVariable.hpp"
#include "I18n.hpp"
#include "Plot.hpp"
#include "PlotGroupHandler.hpp"
#include "Popup.hpp"
#include "ViewerDataHandler.hpp"
#include "imgui.h"

class PlotEditWindow
{
   public:
	PlotEditWindow(PlotHandler* plotHandler, PlotGroupHandler* plotGroupHandler, VariableHandler* variableHandler, ViewerDataHandler* viewerDataHandler) : plotHandler(plotHandler), plotGroupHandler(plotGroupHandler), variableHandler(variableHandler), viewerDataHandler(viewerDataHandler)
	{
		selectVariableWindow = std::make_unique<SelectVariableWindow>(variableHandler, &selection, 1);
	}

	void draw()
	{
		if (showPlotEditWindow)
			ImGui::OpenPopup(I18n::windowTitle("Plot Edit").c_str());

		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(700 * GuiHelper::contentScale, 500 * GuiHelper::contentScale));
		if (ImGui::BeginPopupModal(I18n::windowTitle("Plot Edit").c_str(), &showPlotEditWindow, 0))
		{
			drawPlotEditSettings();

			const float buttonHeight = 25.0f * GuiHelper::contentScale;
			ImGui::SetCursorPos(ImVec2(0, ImGui::GetWindowSize().y - buttonHeight / 2.0f - ImGui::GetFrameHeightWithSpacing()));

			if (ImGui::Button(I18n::tr("Done"), ImVec2(-1, buttonHeight)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
			{
				showPlotEditWindow = false;
				ImGui::CloseCurrentPopup();
			}

			popup.handle();
			selectVariableWindow->draw();
			ImGui::EndPopup();
		}
	}

	void setPlotToEdit(std::shared_ptr<Plot> plot)
	{
		editedPlot = plot;
	}

	void setShowPlotEditWindowState(bool state)
	{
		if (showPlotEditWindow != state)
			stateChanged = true;
		showPlotEditWindow = state;
	}

	void drawPlotEditSettings()
	{
		if (editedPlot == nullptr)
			return;

		std::string name = editedPlot->getName();

		ImGui::Dummy(ImVec2(-1, 5));
		GuiHelper::drawCenteredText(I18n::tr("Plot"));
		ImGui::Separator();

		GuiHelper::drawTextAlignedToSize(I18n::tr("name:"), alignment);
		ImGui::SameLine();

		if (stateChanged)
		{
			ImGui::SetKeyboardFocusHere(0);
			stateChanged = false;
		}

		ImGui::InputText("##name", &name, ImGuiInputTextFlags_None, NULL, NULL);

		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			if (!plotHandler->checkIfPlotExists(name))
			{
				std::string oldName = editedPlot->getName();
				plotHandler->renamePlot(oldName, name);
				plotGroupHandler->renamePlotInAllGroups(oldName, name);
			}
			else
				popup.show(I18n::tr("Error!"), I18n::tr("Plot already exists!"), 1.5f);
		}

		const char* plotTypes[] = {I18n::tr("curve"), I18n::tr("bar"), I18n::tr("table"), I18n::tr("XY")};
		int32_t typeCombo = (int32_t)editedPlot->getType();
		GuiHelper::drawTextAlignedToSize(I18n::tr("type:"), alignment);
		ImGui::SameLine();
		if (ImGui::Combo("##combo", &typeCombo, plotTypes, IM_ARRAYSIZE(plotTypes)))
			editedPlot->setType((Plot::Type)typeCombo);

		if (editedPlot->getType() == Plot::Type::XY)
		{
			GuiHelper::drawTextAlignedToSize(I18n::tr("X-axis variable:"), alignment);
			ImGui::SameLine();

			std::string selectedVariable = "";

			if (selection.empty())
				selectedVariable = editedPlot->getXAxisVariable() ? editedPlot->getXAxisVariable()->getName() : "";
			else
				selectedVariable = *selection.begin();

			ImGui::InputText("##", &selectedVariable, 0, NULL, NULL);

			bool xAxisVariableChanged = false;

			{
				/* A plot names the variables it draws through bare pointers,
				   and the acquisition thread walks the same plots while it
				   works out what to read from the target. */
				auto modelLock = variableHandler->lockModel();

				if (variableHandler->contains(selectedVariable))
				{
					auto* wanted = variableHandler->getVariable(selectedVariable).get();

					/* Only a real change asks for anything: the field is
					   filled from the plot on every frame, so what it holds
					   normally names the variable that is already there. */
					if (editedPlot->getXAxisVariable() != wanted)
					{
						editedPlot->setXAxisVariable(wanted);
						xAxisVariableChanged = true;
					}
				}
			}

			/* An XY plot is drawn against this variable, so it is one of the
			   addresses read from the target, and the list of those is built
			   from the plots. A reader who picks one on a running plot would
			   otherwise get a flat line along the bottom. */
			if (xAxisVariableChanged && viewerDataHandler != nullptr)
				viewerDataHandler->requestRestart();

			ImGui::SameLine();
			if (ImGui::Button(I18n::tr("select..."), ImVec2(65 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
				selectVariableWindow->setShowState(true);
		}

		/* A table is a grid of rows, not a drawing with axes, so there is
		   nothing for a label to sit next to. */
		if (editedPlot->getType() != Plot::Type::TABLE)
			drawAxisLabelSettings();
	}

	void drawAxisLabelSettings()
	{
		/* The field is empty while the axis still carries its automatic label,
		   so the grey text behind the field is that label: what stands in the
		   box is what gets drawn, and an empty box means the grey text is
		   used. Typing over it replaces the label for this plot only. */
		std::string xLabel = editedPlot->getXAxisLabel();
		std::string yLabel = editedPlot->getYAxisLabel();

		GuiHelper::drawTextAlignedToSize(I18n::tr("X-axis label:"), alignment);
		ImGui::SameLine();
		drawAxisLabelInput("##xAxisLabel", xLabel, editedPlot->getDefaultXAxisLabel());
		if (ImGui::IsItemDeactivatedAfterEdit())
			editedPlot->setXAxisLabel(xLabel);

		GuiHelper::drawTextAlignedToSize(I18n::tr("Y-axis label:"), alignment);
		ImGui::SameLine();
		drawAxisLabelInput("##yAxisLabel", yLabel, editedPlot->getDefaultYAxisLabel());
		if (ImGui::IsItemDeactivatedAfterEdit())
			editedPlot->setYAxisLabel(yLabel);
	}

   private:
	/* One axis label field. The label the axis carries on its own is shown as
	   the hint rather than written into the field, so an untouched field stays
	   empty and the plot keeps following whatever it is drawn against. */
	void drawAxisLabelInput(const char* id, std::string& label, const std::string& automatic)
	{
		const std::string hint = automatic.empty() ? std::string(I18n::tr("(no label)")) : automatic;
		ImGui::InputTextWithHint(id, hint.c_str(), &label, 0, NULL, NULL);
	}

	/**
	 * @brief Text alignemnt in front of the input fields
	 *
	 * Wide enough for the longest one, which is the axis variable on an XY
	 * plot.
	 */
	static constexpr size_t alignment = 18;

	bool showPlotEditWindow = false;
	bool stateChanged = false;

	std::shared_ptr<Plot> editedPlot = nullptr;

	PlotHandler* plotHandler;
	PlotGroupHandler* plotGroupHandler;
	VariableHandler* variableHandler;

	/* Asked to rebuild what is read from the target when the variable along
	   the horizontal axis of an XY plot changes. */
	ViewerDataHandler* viewerDataHandler;

	Popup popup;

	std::set<std::string> selection;
	std::unique_ptr<SelectVariableWindow> selectVariableWindow;
};

#endif