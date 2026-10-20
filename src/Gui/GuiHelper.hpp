#ifndef _GUI_HELPER_HPP
#define _GUI_HELPER_HPP

#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

#include "I18n.hpp"
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
 * The colours below follow the same rule, because they are read in far more
 * files than the one that sets them. */
inline float contentScale = 1.0f;

/* ---------------------------------------------------------------------------
 * Theme
 *
 * Two palettes rather than one, because the same green that reads as "running"
 * on a dark panel reads as "washed out" on a light one. The names of the
 * colours do not change with the theme, so a call site asks for `green` and the
 * palette decides which green that is. Nothing outside this file has to know
 * which theme is active.
 *
 * Every colour is a function rather than a variable: a variable would be
 * captured once at the point it is declared and would keep its dark value after
 * the theme changed.
 * ------------------------------------------------------------------------- */

enum class Theme
{
	Dark,
	Light
};

/* Set by Gui::applyTheme, which is the only writer. The default matches the
   palette the application ships with, so a file that draws before the first
   applyTheme call still gets a readable colour. */
inline Theme currentTheme = Theme::Dark;

/* Which theme a stored name refers to. Unknown names fall back to dark, so a
   config written by a newer build cannot leave the interface unreadable. */
inline Theme themeFromName(const std::string& name)
{
	return name == "light" ? Theme::Light : Theme::Dark;
}

inline const char* themeToName(Theme theme)
{
	return theme == Theme::Light ? "light" : "dark";
}

inline ImVec4 themed(ImVec4 dark, ImVec4 light)
{
	return currentTheme == Theme::Light ? light : dark;
}

/* Text that explains rather than reports: the sentence under a table, the unit
 * beside a number, the note that says why a column is empty.
 *
 * These were drawn with `TextDisabled`, which is the colour of something that
 * cannot be used. Beside a dark panel it is close enough to the background that
 * a reader has to lean in, and it carries the wrong meaning besides - nothing
 * written in these lines is disabled. */
inline ImVec4 secondaryTextColour()
{
	return themed((ImVec4)ImColor::HSV(0.0f, 0.0f, 0.74f), (ImVec4)ImColor::HSV(0.0f, 0.0f, 0.34f));
}

/* The same colour, with the text drawn in it. A function rather than a style
 * push at every call site: the call sites are many and the colour is one. */
template <typename... Args>
inline void drawSecondaryText(const char* format, Args... args)
{
	ImGui::PushStyleColor(ImGuiCol_Text, secondaryTextColour());
	ImGui::Text(format, args...);
	ImGui::PopStyleColor();
}

template <typename... Args>
inline void drawSecondaryTextWrapped(const char* format, Args... args)
{
	ImGui::PushStyleColor(ImGuiCol_Text, secondaryTextColour());
	ImGui::PushTextWrapPos(0.0f);
	ImGui::Text(format, args...);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
}

inline ImVec4 white()
{
	return (ImVec4)ImColor::HSV(0.0f, 0.0f, 1.0f);
}

/* Status colours.
 *
 * Two roles have to be served by the same value. Each of these is the face of a
 * button, where the text drawn on top is `ImGuiCol_Text`; every one of them is
 * also used on its own as a text colour. On a dark panel one bright value does
 * both, because the panel behind it is black either way.
 *
 * A light panel separates the two roles. As a face the colour sits behind dark
 * text, so it wants to be clear and mid toned rather than dark, or the button
 * turns into a brown smear. As text it sits on white, so it wants the dark end
 * or it fades out. The light column therefore moves each colour to a mid tone
 * with enough saturation to carry dark text, and the two `Light` variants --
 * which are read as text more often than they are used as a face -- go the
 * other way and darken. */
inline ImVec4 green()
{
	return themed((ImVec4)ImColor::HSV(0.365f, 0.94f, 0.37f), (ImVec4)ImColor::HSV(0.365f, 0.62f, 0.78f));
}

inline ImVec4 greenLight()
{
	return themed((ImVec4)ImColor::HSV(0.365f, 0.94f, 0.57f), (ImVec4)ImColor::HSV(0.365f, 0.72f, 0.34f));
}

inline ImVec4 greenLightDim()
{
	return themed((ImVec4)ImColor::HSV(0.365f, 0.94f, 0.47f), (ImVec4)ImColor::HSV(0.365f, 0.72f, 0.44f));
}

inline ImVec4 red()
{
	return themed((ImVec4)ImColor::HSV(0.0f, 0.95f, 0.72f), (ImVec4)ImColor::HSV(0.0f, 0.58f, 0.90f));
}

/* Read as text far more often than it is used as a face, so on a light panel
   this one goes down to a deep red rather than up to a pale pink. */
inline ImVec4 redLight()
{
	return themed((ImVec4)ImColor::HSV(0.0f, 0.95f, 0.92f), (ImVec4)ImColor::HSV(0.0f, 0.85f, 0.55f));
}

inline ImVec4 redLightDim()
{
	return themed((ImVec4)ImColor::HSV(0.0f, 0.95f, 0.82f), (ImVec4)ImColor::HSV(0.0f, 0.85f, 0.66f));
}

/* The hue is pulled towards yellow on the light panel. The dark value leans
   amber; at a mid tone the same hue turns olive, which is the one reading a
   stop button must not have. */
inline ImVec4 orange()
{
	return themed((ImVec4)ImColor::HSV(0.116f, 0.97f, 0.72f), (ImVec4)ImColor::HSV(0.085f, 0.88f, 0.97f));
}

inline ImVec4 orangeLight()
{
	return themed((ImVec4)ImColor::HSV(0.116f, 0.97f, 0.92f), (ImVec4)ImColor::HSV(0.085f, 0.92f, 0.52f));
}

inline ImVec4 orangeLightDim()
{
	return themed((ImVec4)ImColor::HSV(0.116f, 0.97f, 0.82f), (ImVec4)ImColor::HSV(0.085f, 0.92f, 0.62f));
}

/* The half of a two part readout that carries a name rather than a state. It
   stays neutral so that the coloured half next to it is the one that is read as
   the state. Both halves are button faces, so on a light panel this has to be
   dark enough to stand out from the panel it sits on while still carrying dark
   text, which puts it in the middle of the range rather than at the top. */
inline ImVec4 neutral()
{
	return themed((ImVec4)ImColor::HSV(0.0f, 0.0f, 0.28f), (ImVec4)ImColor::HSV(0.0f, 0.0f, 0.72f));
}

inline ImVec4 neutralLight()
{
	return themed((ImVec4)ImColor::HSV(0.0f, 0.0f, 0.40f), (ImVec4)ImColor::HSV(0.0f, 0.0f, 0.60f));
}

/* A line drawn over the plot area. The white the cursors use on a dark plot is
   invisible on a light one, so the light palette takes it to near black. */
inline ImVec4 cursorLine()
{
	return themed((ImVec4)ImColor::HSV(0.0f, 0.0f, 1.0f), (ImVec4)ImColor::HSV(0.0f, 0.0f, 0.15f));
}

/* The trigger level, which is a marker rather than a value: it has to be seen
   at a glance against the trace it cuts across. Yellow disappears into a white
   plot, so the light palette uses a darker amber. */
inline ImVec4 triggerLine()
{
	return themed((ImVec4)ImColor::HSV(0.14f, 1.0f, 1.0f), (ImVec4)ImColor::HSV(0.09f, 1.0f, 0.60f));
}

/* The band dragged across a plot to mark a span. It is a translucent fill laid
   over the traces, so on a light plot the same cyan washes them out and the
   fill has to be darker to stay visible. */
inline ImVec4 dragSelection()
{
	return themed(ImVec4(0.15f, 0.96f, 0.9f, 0.45f), ImVec4(0.05f, 0.55f, 0.62f, 0.30f));
}

/* The colour a trace is drawn in, given the colour that is stored for it.
 *
 * A variable is handed a random colour when it is created, over the whole
 * 32 bit range, so nothing stops it from being a pale yellow or a near white.
 * On a dark plot that is already a gamble; on a light one such a trace
 * disappears.
 *
 * The stored value is left alone. Lowering the brightness for drawing only
 * means the project file keeps the colour the user picked or was given, the
 * colour picker in the variable table and the plot tree keeps showing that
 * colour, and a project moved between themes keeps its identity. What changes
 * is only how it is put on the plot.
 *
 * The adjustment is a ceiling on brightness rather than a fixed darkening, so a
 * colour that is already dark comes through untouched and a light one is pulled
 * down until it separates from the white behind it. Saturation is nudged up as
 * the brightness comes down, which keeps a pastel from turning grey.
 *
 * Takes the four components rather than an ImVec4 because the value comes from
 * Variable::Color at most call sites, which is a plain struct with r/g/b/a. */
inline ImVec4 traceColor(float r, float g, float b, float a = 1.0f)
{
	if (currentTheme != Theme::Light)
		return ImVec4(r, g, b, a);

	const float brightnessMax = 0.55f;

	float hue = 0.0f;
	float saturation = 0.0f;
	float brightness = 0.0f;
	ImGui::ColorConvertRGBtoHSV(r, g, b, hue, saturation, brightness);

	if (brightness <= brightnessMax)
		return ImVec4(r, g, b, a);

	/* A colour with no saturation is grey, and raising the saturation of grey
	   only introduces a hue that was never asked for. */
	if (saturation > 0.0f)
		saturation = saturation + (1.0f - saturation) * 0.55f;

	ImVec4 adjusted = (ImVec4)ImColor::HSV(hue, saturation, brightnessMax);
	adjusted.w = a;

	return adjusted;
}

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
 * @param alignTo width to pad to, counted in the characters of the English
 * label the call site was written for
 */
void drawTextAlignedToSize(std::string&& text, size_t alignTo);

/**
 * @brief A label padded with spaces so that it fills the width of a column
 *
 * The column is measured in the characters of the English label the call site
 * was written for, and turned into pixels here. Padding by character count
 * instead only holds while every character is the same width, which was true
 * of the embedded font and stops being true the moment the label is Chinese:
 * the same words take fewer characters and more pixels, and the control to the
 * right of them lands somewhere else on every line.
 *
 * @param text the label, already in the language being drawn
 * @param column width to pad to, counted in English characters
 * @return std::string the label, padded; unchanged when it is already wider
 */
std::string padToColumn(const std::string& text, size_t column);

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

/* A number written in fixed point with its unit appended.
 *
 * Kept apart from the drawing below because a table cell cannot use it: the row
 * macro pairs a label and a value on one line, which is the one thing a cell
 * must not do - the value has to go into its own column. */
template <typename T>
std::string formatNumber(T number, std::string unit = "", size_t decimalPlaces = 5)
{
	std::ostringstream formattedNum;
	formattedNum << std::fixed << std::setprecision(decimalPlaces) << number;
	return formattedNum.str() + unit;
}

/* Marks the half of a reading that has crossed a threshold. On a dark panel
   this is the default text colour, so the caller passes the colour it wants and
   the default here only has to work for the palette in use at the time.
 *
 * The description is written as the English label padded with the spaces that
 * take it out to its column, and the translation is padded to the same width
 * here rather than in the table. Counting the spaces in the table would count
 * them in whichever font draws the translation, and a space in the Chinese font
 * is not the width of a space in the Latin one. */
template <typename T>
void drawDescriptionWithNumber(const char* description, T number, std::string unit = "", size_t decimalPlaces = 5, float threshold = std::nan(""), ImVec4 thresholdExceededColor = ImVec4(-1.0f, -1.0f, -1.0f, 1.0f))
{
	const std::string label = padToColumn(I18n::tr(description), std::strlen(description));

	if (threshold != std::nan("") && number > threshold)
	{
		/* A negative alpha tells the caller apart from a colour it chose
		   itself. Read here rather than in the default argument so that the
		   theme is the one in force when the row is drawn. */
		if (thresholdExceededColor.x < 0.0f)
			thresholdExceededColor = redLight();

		ImGui::TextColored(thresholdExceededColor, "%s", label.c_str());
	}
	else
		ImGui::Text("%s", label.c_str());
	ImGui::SameLine();
	ImGui::Text("%s", formatNumber(number, unit, decimalPlaces).c_str());
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