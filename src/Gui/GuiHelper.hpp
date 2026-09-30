#ifndef _GUI_HELPER_HPP
#define _GUI_HELPER_HPP

#include <cstdint>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

#include "ImguiPlugins.hpp"
#include "imgui.h"

namespace GuiHelper
{
/* The scale of the display the window sits on. Written once by Gui::mainThread
 * and read by every file that sizes a widget, so it has to be one object shared
 * by all of them. `inline` gives it a single definition across translation
 * units; inside a header a plain `static` would give each file its own copy, and
 * the file that does the writing is not the file that does most of the reading.
 *
 * The colours below are constant, so a copy each would only waste space, but
 * they follow the same rule so that a later change cannot reintroduce the
 * split. */
inline float contentScale = 1.0f;

inline ImVec4 white = (ImVec4)ImColor::HSV(0.0f, 0.0f, 1.0f);

inline ImVec4 green = (ImVec4)ImColor::HSV(0.365f, 0.94f, 0.37f);
inline ImVec4 greenLight = (ImVec4)ImColor::HSV(0.365f, 0.94f, 0.57f);
inline ImVec4 greenLightDim = (ImVec4)ImColor::HSV(0.365f, 0.94f, 0.47f);

inline ImVec4 red = (ImVec4)ImColor::HSV(0.0f, 0.95f, 0.72f);
inline ImVec4 redLight = (ImVec4)ImColor::HSV(0.0f, 0.95f, 0.92f);
inline ImVec4 redLightDim = (ImVec4)ImColor::HSV(0.0f, 0.95f, 0.82f);

inline ImVec4 orange = (ImVec4)ImColor::HSV(0.116f, 0.97f, 0.72f);
inline ImVec4 orangeLight = (ImVec4)ImColor::HSV(0.116f, 0.97f, 0.92f);
inline ImVec4 orangeLightDim = (ImVec4)ImColor::HSV(0.116f, 0.97f, 0.82f);

/* The half of a two part readout that carries a name rather than a state. It
   stays neutral so that the coloured half next to it is the one that is read as
   the state. */
inline ImVec4 neutral = (ImVec4)ImColor::HSV(0.0f, 0.0f, 0.28f);
inline ImVec4 neutralLight = (ImVec4)ImColor::HSV(0.0f, 0.0f, 0.40f);

/**
 * @brief Convert int32_t to hexadecilam string
 *
 * @param var
 * @return std::string
 */
std::string intToHexString(uint32_t var);

/**
 * @brief Text of a number with the trailing zeroes removed, for an input field
 *
 * @param number
 * @return std::string
 */
std::string numberToString(double number);

/**
 * @brief Draw window-centered text
 *
 * @param text
 */
void drawCenteredText(std::string&& text);

/**
 * @brief draw text with alignment to x fields of characters
 *
 * @param text
 * @param alignTo amount of characters to align the text to
 */
void drawTextAlignedToSize(std::string&& text, size_t alignTo);

/**
 * @brief Parse hex string to a decimal number
 *
 * @param hexStr
 * @return uint32_t result
 */
uint32_t hexStringToDecimal(const std::string& hexStr);

/**
 * @brief Show delete popup
 *
 * @param text text to be shown on the button
 * @param name name of the item to be deleted
 * @return std::optional<std::string> if button clicked returns name, otherwise std::nullopt
 */
std::string convertProjectPathToAbsolute(const std::string* relativePath, const std::string* projectConfigPath);

std::optional<std::string> showDeletePopup(const char* text, const std::string& name);

void showQuestionBox(const char* id, const char* question, std::function<void()> onYes, std::function<void()> onNo, std::function<void()> onCancel);

template <typename T>
void drawInputText(const char* id, T variable, std::function<void(std::string)> valueChanged)
{
	std::string str = std::to_string(variable);
	if (ImGui::InputText(id, &str, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL) || ImGui::IsMouseClicked(0))
		if (valueChanged)
			valueChanged(str);
}

template <typename T>
void drawDescriptionWithNumber(const char* description, T number, std::string unit = "", size_t decimalPlaces = 5, float threshold = std::nan(""), ImVec4 thresholdExceededColor = {0.0f, 0.0f, 0.0f, 1.0f})
{
	if (threshold != std::nan("") && number > threshold)
		ImGui::TextColored(thresholdExceededColor, "%s", description);
	else
		ImGui::Text("%s", description);
	ImGui::SameLine();
	std::ostringstream formattedNum;
	formattedNum << std::fixed << std::setprecision(decimalPlaces) << number;
	ImGui::Text("%s", (formattedNum.str() + unit).c_str());
}

template <typename T>
T convertStringToNumber(std::string& str)
{
	T result;
	std::istringstream ss(str);
	ss >> result;
	return result;
}

}  // namespace GuiHelper

#endif