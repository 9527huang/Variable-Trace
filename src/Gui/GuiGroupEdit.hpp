#pragma once

#include <string>
#include <vector>

#include "GuiHelper.hpp"
#include "I18n.hpp"
#include "Plot.hpp"
#include "PlotGroupHandler.hpp"
#include "Popup.hpp"
#include "imgui.h"

class GroupEditWindow
{
   public:
	GroupEditWindow(PlotGroupHandler* plotGroupHandler) : plotGroupHandler(plotGroupHandler)
	{
	}

	void draw()
	{
		if (showGroupEditWindow)
			ImGui::OpenPopup(I18n::tr("Group Edit"));

		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(700 * GuiHelper::contentScale, 500 * GuiHelper::contentScale));
		if (ImGui::BeginPopupModal(I18n::tr("Group Edit"), &showGroupEditWindow, 0))
		{
			drawGroupEditSettings();

			const float buttonHeight = 25.0f * GuiHelper::contentScale;
			ImGui::SetCursorPos(ImVec2(0, ImGui::GetWindowSize().y - buttonHeight / 2.0f - ImGui::GetFrameHeightWithSpacing()));

			if (ImGui::Button(I18n::tr("Done"), ImVec2(-1, buttonHeight)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
			{
				showGroupEditWindow = false;
				ImGui::CloseCurrentPopup();
			}

			popup.handle();
			ImGui::EndPopup();
		}
	}

	void setGroupToEdit(std::shared_ptr<PlotGroup> group)
	{
		editedGroup = group;
	}

	void setShowGroupEditWindowState(bool state)
	{
		if (showGroupEditWindow != state)
			stateChanged = true;
		showGroupEditWindow = state;
	}

	void drawGroupEditSettings()
	{
		if (editedGroup == nullptr)
			return;

		std::string name = editedGroup->getName();

		ImGui::Dummy(ImVec2(-1, 5));
		GuiHelper::drawCenteredText(I18n::tr("Group"));
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
			if (!plotGroupHandler->checkIfGroupExists(name))
			{
				std::string oldName = editedGroup->getName();
				plotGroupHandler->renameGroup(oldName, name);
			}

			else
				popup.show(I18n::tr("Error!"), I18n::tr("Group already exists!"), 1.5f);
		}

		/* PARENT */
		GuiHelper::drawTextAlignedToSize(I18n::tr("parent:"), alignment);
		ImGui::SameLine();

		/* Every group is offered except this one and the groups below it, because
		   nesting a group in its own child would cut the whole branch off the
		   tree. Index 0 is the top level, which is not a group. */
		std::vector<std::string> candidates{I18n::tr("top level")};

		for (const auto& [candidateName, candidate] : *plotGroupHandler)
		{
			if (candidateName == editedGroup->getName() || plotGroupHandler->isDescendantOf(candidateName, editedGroup->getName()))
				continue;

			candidates.push_back(candidateName);
		}

		int32_t currentParent = 0;

		for (size_t index = 1; index < candidates.size(); index++)
		{
			if (candidates[index] == editedGroup->getParentName())
				currentParent = static_cast<int32_t>(index);
		}

		if (ImGui::BeginCombo("##parent", candidates[static_cast<size_t>(currentParent)].c_str()))
		{
			for (size_t index = 0; index < candidates.size(); index++)
			{
				const bool isCurrent = static_cast<int32_t>(index) == currentParent;

				if (ImGui::Selectable(candidates[index].c_str(), isCurrent))
					plotGroupHandler->moveGroup(editedGroup->getName(), index == 0 ? "" : candidates[index]);

				if (isCurrent)
					ImGui::SetItemDefaultFocus();
			}

			ImGui::EndCombo();
		}
	}

   private:
	/**
	 * @brief Text alignemnt in front of the input fields
	 *
	 */
	static constexpr size_t alignment = 18;

	bool showGroupEditWindow = false;

	PlotGroupHandler* plotGroupHandler;
	std::shared_ptr<PlotGroup> editedGroup = nullptr;

	Popup popup;

	bool stateChanged = false;
};
