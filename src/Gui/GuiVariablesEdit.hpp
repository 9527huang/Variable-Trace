#ifndef _GUI_VARIABLESEDIT_HPP
#define _GUI_VARIABLESEDIT_HPP

#include <algorithm>
#include <optional>
#include <vector>

#include "GuiHelper.hpp"
#include "Popup.hpp"
#include "Variable.hpp"
#include "VariableHandler.hpp"
#include "imgui.h"

class VariableEditWindow
{
   public:
	VariableEditWindow(VariableHandler* variableHandler) : variableHandler(variableHandler)
	{
		selectVariableWindow = std::make_unique<SelectVariableWindow>(variableHandler, &selection, 1);
		selectVariableWindowBase = std::make_unique<SelectVariableWindow>(variableHandler, &selectionBase, 2);
	}

	void draw()
	{
		if (showVariableEditWindow)
			ImGui::OpenPopup("Variable Edit");

		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(800 * GuiHelper::contentScale, 500 * GuiHelper::contentScale));
		if (ImGui::BeginPopupModal("Variable Edit", &showVariableEditWindow, 0))
		{
			drawVariableEditSettings();

			const float buttonHeight = 25.0f * GuiHelper::contentScale;
			ImGui::SetCursorPos(ImVec2(0, ImGui::GetWindowSize().y - buttonHeight / 2.0f - ImGui::GetFrameHeightWithSpacing()));

			if (ImGui::Button("Done", ImVec2(-1, buttonHeight)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
			{
				showVariableEditWindow = false;
				selection.clear();
				selectionBase.clear();
				ImGui::CloseCurrentPopup();
			}

			popup.handle();
			selectVariableWindow->draw();
			selectVariableWindowBase->draw();
			ImGui::EndPopup();
		}
	}

	void setVariableToEdit(std::shared_ptr<Variable> variable)
	{
		editedVariable = variable;
	}

	void setShowVariableEditWindowState(bool state)
	{
		if (showVariableEditWindow != state)
			stateChanged = true;
		showVariableEditWindow = state;
	}

	void drawVariableEditSettings()
	{
		if (editedVariable == nullptr)
			return;

		std::string name = editedVariable->getName();
		std::string address = std::string("0x") + std::string(GuiHelper::intToHexString(editedVariable->getAddress()));
		std::string size = std::to_string(editedVariable->getSize());
		std::string shift = std::to_string(editedVariable->getShift());
		std::string mask = std::string("0x") + std::string(GuiHelper::intToHexString(editedVariable->getMask()));
		bool shouldUpdateFromElf = editedVariable->getShouldUpdateFromElf();
		bool selectNameManually = editedVariable->getIsTrackedNameDifferent();

		ImGui::Dummy(ImVec2(-1, 5));
		GuiHelper::drawCenteredText("General");
		ImGui::Separator();

		if (stateChanged)
		{
			ImGui::SetKeyboardFocusHere(0);
			stateChanged = false;
		}
		/* NAME */
		GuiHelper::drawTextAlignedToSize("name:", alignment);
		ImGui::SameLine();
		ImGui::InputText("##name", &name, ImGuiInputTextFlags_None, NULL, NULL);
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			if (!variableHandler->contains(name))
			{
				variableHandler->renameVariable(editedVariable->getName(), name);
			}
			else
				popup.show("Error!", "Variable already exists!", 1.5f);
		}

		ImGui::SameLine();
		ImGui::HelpMarker("Name has to be unique - it can be different from tracked variable name if you specify it manually below.");

		/* TRACKED NAME */
		ImGui::BeginDisabled(!shouldUpdateFromElf);

		GuiHelper::drawTextAlignedToSize("specify tracked name:", alignment);
		ImGui::SameLine();
		if (ImGui::Checkbox("##selectNameManually", &selectNameManually))
			editedVariable->setIsTrackedNameDifferent(selectNameManually);

		ImGui::SameLine();
		ImGui::HelpMarker("Check if you'd like to specify a different variable name that will be sampled.");

		ImGui::EndDisabled();

		ImGui::BeginDisabled(!selectNameManually);

		GuiHelper::drawTextAlignedToSize("tracked variable:", alignment);
		ImGui::SameLine();

		std::string trackedVarName = selection.empty() ? editedVariable->getTrackedName() : *selection.begin();

		if (ImGui::InputText("##trackedVarName", &trackedVarName, ImGuiInputTextFlags_None, NULL, NULL) || editedVariable->getTrackedName() != trackedVarName)
		{
			if (variableHandler->contains(trackedVarName))
			{
				editedVariable->setTrackedName(trackedVarName);
				editedVariable->setAddress(variableHandler->getVariable(trackedVarName)->getAddress());
				editedVariable->setType(variableHandler->getVariable(trackedVarName)->getType());
				selection.clear();
			}
		}

		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			if (!variableHandler->contains(trackedVarName))
				popup.show("Error!", "Tracked variable doesn't exist!", 1.5f);
		}

		ImGui::SameLine();
		if (ImGui::Button("select...", ImVec2(65 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
			selectVariableWindow->setShowState(true);

		ImGui::SameLine();
		ImGui::HelpMarker("Select or type an imported variable name from the *.elf file.");

		ImGui::EndDisabled();

		/* SHOULD UPDATE FROM ELF */
		GuiHelper::drawTextAlignedToSize("update from *.elf:", alignment);
		ImGui::SameLine();
		if (ImGui::Checkbox("##shouldUpdateFromElf", &shouldUpdateFromElf))
		{
			editedVariable->setShouldUpdateFromElf(shouldUpdateFromElf);
			editedVariable->setIsTrackedNameDifferent(false);
		}
		ImGui::SameLine();
		ImGui::HelpMarker("Check if the variable address and size should be automatically updated from *.elf file.");

		/* ADDRESS */
		ImGui::BeginDisabled(shouldUpdateFromElf);

		GuiHelper::drawTextAlignedToSize("address:", alignment);
		ImGui::SameLine();
		if (ImGui::InputText("##address", &address, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase, NULL, NULL))
		{
			uint32_t addressDec = GuiHelper::hexStringToDecimal(address);
			editedVariable->setAddress(addressDec);
		}
		/* BASE TYPE*/
		GuiHelper::drawTextAlignedToSize("base type:", alignment);
		ImGui::SameLine();
		int32_t type = static_cast<int32_t>(editedVariable->getType());

		if (ImGui::Combo("##varType", &type, Variable::types, IM_ARRAYSIZE(Variable::types)))
		{
			editedVariable->setType(static_cast<Variable::Type>(type));
		}

		ImGui::EndDisabled();

		/* WRITE LIMITS */
		GuiHelper::drawTextAlignedToSize("write limits:", alignment);
		ImGui::SameLine();

		bool writeLimitsEnabled = editedVariable->getWriteLimitsEnabled();

		if (ImGui::Checkbox("##writeLimitsEnabled", &writeLimitsEnabled))
		{
			/* The bounds are kept as they are, because turning the guard back on
			   should restore the range the user last typed rather than zeroes. */
			editedVariable->setWriteLimits(writeLimitsEnabled, editedVariable->getWriteMin(), editedVariable->getWriteMax());
		}

		ImGui::SameLine();
		ImGui::HelpMarker("When enabled, writing values outside the specified range will be rejected.");

		ImGui::BeginDisabled(!writeLimitsEnabled);

		std::string writeMin = GuiHelper::numberToString(editedVariable->getWriteMin());
		std::string writeMax = GuiHelper::numberToString(editedVariable->getWriteMax());

		GuiHelper::drawTextAlignedToSize("min:", alignment);
		ImGui::SameLine();
		if (ImGui::InputText("##writeLimitMin", &writeMin, ImGuiInputTextFlags_CharsDecimal, NULL, NULL))
			editedVariable->setWriteLimits(true, GuiHelper::convertStringToNumber<double>(writeMin), editedVariable->getWriteMax());

		if (ImGui::IsItemDeactivatedAfterEdit() && editedVariable->getWriteMin() > editedVariable->getWriteMax())
			popup.show("Error!", "Min value cannot be greater than max value!", 1.5f);

		GuiHelper::drawTextAlignedToSize("max:", alignment);
		ImGui::SameLine();
		if (ImGui::InputText("##writeLimitMax", &writeMax, ImGuiInputTextFlags_CharsDecimal, NULL, NULL))
			editedVariable->setWriteLimits(true, editedVariable->getWriteMin(), GuiHelper::convertStringToNumber<double>(writeMax));

		if (ImGui::IsItemDeactivatedAfterEdit() && editedVariable->getWriteMax() < editedVariable->getWriteMin())
			popup.show("Error!", "Max value cannot be less than min value!", 1.5f);

		ImGui::EndDisabled();

		/* POSTPROCESSING */
		ImGui::Dummy(ImVec2(-1, 5));
		GuiHelper::drawCenteredText("Postprocessing");
		ImGui::SameLine();
		ImGui::HelpMarker("Applies transformation [(value >> shift) & mask] after sampling and before interpretation.");
		ImGui::Separator();

		GuiHelper::drawTextAlignedToSize("shift right:", alignment);
		ImGui::SameLine();
		if (ImGui::InputText("##shift", &shift, ImGuiInputTextFlags_CharsDecimal, NULL, NULL))
		{
			uint32_t shiftDec = GuiHelper::convertStringToNumber<uint32_t>(shift);
			shiftDec = std::clamp<uint32_t>(shiftDec, 0, (editedVariable->getSize() * 8) - 1);
			editedVariable->setShift(shiftDec);
		}

		GuiHelper::drawTextAlignedToSize("mask:", alignment);
		ImGui::SameLine();
		if (ImGui::InputText("##mask", &mask, ImGuiInputTextFlags_None, NULL, NULL))
		{
			uint32_t maskDec = GuiHelper::hexStringToDecimal(mask);
			editedVariable->setMask(maskDec);
		}

		/* INTERPRETATION */
		ImGui::Dummy(ImVec2(-1, 5));
		GuiHelper::drawCenteredText("Interpretation");
		ImGui::SameLine();
		ImGui::HelpMarker("Interpret integer numbers as fixed point variables.");
		ImGui::Separator();

		GuiHelper::drawTextAlignedToSize("type:", alignment);
		ImGui::SameLine();
		int32_t highLeveltype = static_cast<int32_t>(editedVariable->getHighLevelType());

		if (ImGui::Combo("##varHighLevelType", &highLeveltype, Variable::highLevelTypes, IM_ARRAYSIZE(Variable::highLevelTypes)))
		{
			editedVariable->setHighLevelType(static_cast<Variable::HighLevelType>(highLeveltype));
		}

		if (editedVariable->isFractional())
		{
			Variable::Fractional fractional = editedVariable->getFractional();

			std::string fractionalBits = std::to_string(fractional.fractionalBits);
			bool shouldUpdate = false;

			GuiHelper::drawTextAlignedToSize("fractional bits:", alignment);
			ImGui::SameLine();
			if (ImGui::InputText("##fractional", &fractionalBits, ImGuiInputTextFlags_CharsDecimal, NULL, NULL))
				shouldUpdate = true;

			/* BASE */

			std::string base = "";

			if (!selectionBase.empty())
				base = *selectionBase.begin();
			else if (fractional.baseVariable != nullptr)
				base = fractional.baseVariable->getName();
			else
				base = std::to_string(fractional.base);

			GuiHelper::drawTextAlignedToSize("base:", alignment);
			ImGui::SameLine();

			if (ImGui::InputText("##base", &base, ImGuiInputTextFlags_None, NULL, NULL))
				shouldUpdate = true;

			if (fractional.baseVariable != nullptr && base != fractional.baseVariable->getName())
				shouldUpdate = true;

			ImGui::SameLine();
			ImGui::PushID("fractional");
			if (ImGui::Button("select...", ImVec2(65 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
				selectVariableWindowBase->setShowState(true);
			ImGui::PopID();
			ImGui::SameLine();
			ImGui::HelpMarker("Either type a new base (multiplier), or select a variable defined as base from the list of imported variables.");

			if (shouldUpdate)
			{
				fractional.fractionalBits = std::clamp<uint32_t>(GuiHelper::convertStringToNumber<uint32_t>(fractionalBits), 0, (editedVariable->getSize() * 8) - 1);

				if (variableHandler->contains(base))
					fractional.baseVariable = variableHandler->getVariable(base).get();
				else
				{
					fractional.base = GuiHelper::convertStringToNumber<double>(base);
					fractional.baseVariable = nullptr;
				}
				selectionBase.clear();
				// TODO check if not zero or negative and set to 1 in such cases and show a popup
				editedVariable->setFractional(fractional);
			}
		}

		if (editedVariable->isEnum())
			drawEnumLabelsEditor();
	}

	/* The label list of a custom enumeration. An 'enum' interpretation whose
	   labels come from the symbol file shows the same table, so that a name can
	   be corrected without switching the interpretation first. */
	void drawEnumLabelsEditor()
	{
		ImGui::Dummy(ImVec2(-1, 3));
		GuiHelper::drawTextAlignedToSize("fields:", alignment);
		ImGui::SameLine();
		ImGui::HelpMarker("Name the values this variable can take. A value without a name is shown as a number.");

		std::vector<Variable::EnumLabel> labels = editedVariable->getEnumLabels();
		std::optional<size_t> indexToRemove;

		const float rowHeight = 23.0f * GuiHelper::contentScale;

		if (ImGui::BeginTable("##Enum Values", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 4 * rowHeight)))
		{
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 90 * GuiHelper::contentScale);
			ImGui::TableSetupColumn("##", ImGuiTableColumnFlags_WidthFixed, 60 * GuiHelper::contentScale);
			ImGui::TableHeadersRow();

			for (size_t index = 0; index < labels.size(); index++)
			{
				ImGui::TableNextRow();
				ImGui::PushID(static_cast<int>(index));

				ImGui::TableSetColumnIndex(0);
				std::string label = labels[index].label;

				if (ImGui::InputText("##customEnumLabel", &label, ImGuiInputTextFlags_None, NULL, NULL))
					labels[index].label = label;

				if (ImGui::IsItemDeactivatedAfterEdit())
				{
					bool duplicated = false;

					for (size_t other = 0; other < labels.size(); other++)
					{
						if (other != index && labels[other].label == labels[index].label)
							duplicated = true;
					}

					if (labels[index].label.empty())
						popup.show("Error!", "Label cannot be empty!", 1.5f);
					else if (duplicated)
						popup.show("Error!", "Label already exists!", 1.5f);
				}

				ImGui::TableSetColumnIndex(1);
				std::string value = std::to_string(labels[index].value);

				/* The value field is signed, so the minus sign has to be allowed
				   through; CharsDecimal accepts digits, a sign and a dot. */
				if (ImGui::InputText("##customEnumValue", &value, ImGuiInputTextFlags_CharsDecimal, NULL, NULL))
					labels[index].value = static_cast<int64_t>(GuiHelper::convertStringToNumber<double>(value));

				if (ImGui::IsItemDeactivatedAfterEdit() && value.find_first_not_of("0123456789+-.") != std::string::npos)
					popup.show("Error!", "Value must be a valid integer!", 1.5f);

				ImGui::TableSetColumnIndex(2);
				ImGui::PushID("del");

				if (ImGui::Button("Del", ImVec2(-1, rowHeight)))
					indexToRemove = index;

				ImGui::PopID();
				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		if (ImGui::Button("Add", ImVec2(120 * GuiHelper::contentScale, rowHeight)))
			labels.push_back({"", 0});

		if (indexToRemove.has_value())
			labels.erase(labels.begin() + static_cast<std::ptrdiff_t>(*indexToRemove));

		/* Writing the list back on every frame would be wasted work and would
		   make the labels change under a caller that only meant to look. */
		if (labels != editedVariable->getEnumLabels())
			editedVariable->setEnumLabels(labels);
	}

   private:
	/**
	 * @brief Text alignemnt in front of the input fields
	 *
	 */
	static constexpr size_t alignment = 22;

	bool showVariableEditWindow = false;
	bool stateChanged = false;

	std::shared_ptr<Variable> editedVariable = nullptr;

	VariableHandler* variableHandler;

	Popup popup;

	std::set<std::string> selection{};
	std::unique_ptr<SelectVariableWindow> selectVariableWindow;

	std::set<std::string> selectionBase{};
	std::unique_ptr<SelectVariableWindow> selectVariableWindowBase;
};

#endif