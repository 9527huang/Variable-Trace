#include "GuiHelper.hpp"

#include <filesystem>

#include "I18n.hpp"

std::string GuiHelper::intToHexString(uint32_t var)
{
	std::stringstream ss;
	ss << std::hex << var;
	return ss.str();
}

void GuiHelper::drawCenteredText(std::string&& text)
{
	ImGui::SetCursorPosX((ImGui::GetWindowSize().x - ImGui::CalcTextSize(text.c_str()).x) * 0.5f);
	ImGui::Text("%s", text.c_str());
}

std::string GuiHelper::padToColumn(const std::string& text, size_t column)
{
	/* The call site asks for a column `column` characters wide, and every call
	   site counts those characters against the English label it was written
	   for. What the count stands for is a width, so it is turned into one and
	   the padding is measured against that. */
	const float latinCharacterWidth = ImGui::CalcTextSize("M").x;
	const float columnWidth = latinCharacterWidth * static_cast<float>(column);

	const float textWidth = ImGui::CalcTextSize(text.c_str()).x;
	const float spaceWidth = ImGui::CalcTextSize(" ").x;

	int spacesToAdd = 0;

	if (spaceWidth > 0.0f)
		spacesToAdd = static_cast<int>((columnWidth - textWidth) / spaceWidth + 0.5f);

	/* A label that is already wider than its column is left as it is, which is
	   what the character-count version did: the control after it moves right
	   and the rest of the rows stay put. */
	if (spacesToAdd < 0)
		spacesToAdd = 0;

	return text + std::string(static_cast<size_t>(spacesToAdd), ' ');
}

void GuiHelper::drawTextAlignedToSize(std::string&& text, size_t alignTo)
{
	ImGui::Text("%s", padToColumn(text, alignTo).c_str());
}

uint32_t GuiHelper::hexStringToDecimal(const std::string& hexStr)
{
	std::string cleanHexStr = hexStr;

	if (hexStr.size() >= 2 && (hexStr[0] == '0' && (hexStr[1] == 'x' || hexStr[1] == 'X')))
		cleanHexStr = hexStr.substr(2);

	uint32_t decimalValue;
	std::stringstream ss;
	ss << std::hex << cleanHexStr;
	ss >> decimalValue;

	return decimalValue;
}


std::string GuiHelper::numberToString(double number)
{
	/* std::to_string fixes the decimals at six, which turns a limit of 100 into
	   "100.000000" in the field the user has to edit. Printing the shortest text
	   that reads back as the same number keeps the field tidy. */
	std::ostringstream stream;
	stream << std::setprecision(15) << number;

	std::string text = stream.str();

	if (text.find('.') == std::string::npos)
		return text;

	while (!text.empty() && text.back() == '0')
		text.pop_back();

	if (!text.empty() && text.back() == '.')
		text.pop_back();

	return text;
}

/* Both arguments are read only: the path is resolved against the folder of the
   project file, and neither is changed, so a caller that holds them as const
   can use this as well. */
std::string GuiHelper::convertProjectPathToAbsolute(const std::string* relativePath, const std::string* projectConfigPath)
{
	if (relativePath->empty())
		return "";

	try
	{
		// Convert relative path to absolute path based on project file location
		std::filesystem::path absPath = std::filesystem::absolute(std::filesystem::path(*projectConfigPath).parent_path() / (*relativePath));
		return absPath.string();
	}
	catch (std::filesystem::filesystem_error& e)
	{
		// logger->error("Failed to convert path to absolute: {}", e.what()); // TODO
		return "";
	}
}

std::optional<std::string> GuiHelper::showDeletePopup(const char* text, const std::string& name)
{
	ImGui::PushID(name.c_str());
	if (ImGui::BeginPopupContextItem(text))
	{
		if (ImGui::Button(text))
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			ImGui::PopID();
			return name;
		}
		ImGui::EndPopup();
	}
	ImGui::PopID();
	return std::nullopt;
}

void GuiHelper::showQuestionBox(const char* id, const char* question, std::function<void()> onYes, std::function<void()> onNo, std::function<void()> onCancel)
{
	float buttonWidth = 120.0f * GuiHelper::contentScale;

	/* The caller opens the box by the same id, and that call site goes through
	   the table, so this one has to as well or the two would no longer name the
	   same popup and it would never appear. */
	id = I18n::tr(id);

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (ImGui::BeginPopupModal(id, NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("%s", question);
		ImGui::Separator();
		if (ImGui::Button(I18n::tr("Yes"), ImVec2(buttonWidth, 0)))
		{
			onYes();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(I18n::tr("No"), ImVec2(buttonWidth, 0)))
		{
			onNo();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(I18n::tr("Cancel"), ImVec2(buttonWidth, 0)))
		{
			onCancel();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}
