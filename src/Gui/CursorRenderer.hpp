#ifndef _CURSORRENDERER_HPP
#define _CURSORRENDERER_HPP

#include <string>

#include "GuiHelper.hpp"
#include "Plot.hpp"
#include "imgui.h"
#include "implot.h"

/**
 * @brief Draws the cursors of a plot and the readings that go with them
 *
 * The cursors live in two independent directions. An X cursor is a vertical
 * line and measures along the bottom of the plot; a Y cursor is a horizontal
 * line and measures up the side. Each direction carries a pair of them,
 * because a single line can say where something is while only two of them can
 * say how far apart two things are.
 *
 * Nothing here decides whether the cursors should be shown at all: that is the
 * plot's cursor mode, which the panels in the two plot tabs set. This class
 * only draws what it is handed.
 */
class CursorRenderer
{
   public:
	/* The colour a cursor is drawn in. The two directions are different
	   colours so that a plot carrying both stays readable, and each direction
	   keeps one colour for both of its lines with the second line dimmed. */
	static constexpr ImVec4 xCursorColour{1.0f, 0.0f, 0.0f, 1.0f};
	static constexpr ImVec4 yCursorColour{0.0f, 1.0f, 1.0f, 1.0f};

	/**
	 * @brief Draws one cursor line and takes back the place it was dragged to
	 *
	 * @param id unique per plot, so that dragging one cursor does not move another
	 * @param marker the cursor being drawn
	 * @param vertical true for an X cursor, false for a Y cursor
	 * @param colour the colour of this cursor's line
	 */
	static void drawCrosshair(uint32_t id, Plot::Marker& marker, bool vertical, const ImVec4& colour)
	{
		ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 1.0f);

		/* The line is drawn from a copy, because ImPlot moves the value it is
		   given as the user drags, and the result has to be written back to
		   the marker or the cursor would return to where it was. */
		double position = marker.getValue();

		if (vertical)
			ImPlot::DragLineX(static_cast<int>(id), &position, colour);
		else
			ImPlot::DragLineY(static_cast<int>(id), &position, colour);

		marker.setValue(position);

		ImPlot::PopStyleVar();
	}

	/**
	 * @brief Draws a pair of X cursors and the readings they give
	 *
	 * @param plot the plot being drawn
	 * @param limits what the plot currently shows
	 */
	static void drawX(Plot* plot, const ImPlotRect& limits)
	{
		drawX(plot, limits, 0);
	}

	/**
	 * @brief Draws a pair of Y cursors and the readings they give
	 *
	 * @param plot the plot being drawn
	 * @param limits what the plot currently shows
	 */
	static void drawY(Plot* plot, const ImPlotRect& limits)
	{
		drawY(plot, limits, 2);
	}

	/**
	 * @brief Prepares the cursors of a plot that is about to start drawing them
	 *
	 * Switching the cursors on gives them no position, so they are put back to
	 * where a fresh pair goes. Called by the panel when the switch is flipped,
	 * which is the one moment the user is asking for them to be placed again.
	 *
	 * @param plot the plot whose cursors are being switched on
	 */
	static void resetCursors(Plot* plot)
	{
		plot->markerX0.setState(false);
		plot->markerX1.setState(false);
		plot->markerY0.setState(false);
		plot->markerY1.setState(false);

		plot->markerX0.setValue(0.0);
		plot->markerX1.setValue(0.0);
		plot->markerY0.setValue(0.0);
		plot->markerY1.setValue(0.0);
	}

	/**
	 * @brief The readings a pair of cursors produces, drawn where they do not overlap
	 *
	 * @param first the cursor that is placed first along the axis
	 * @param second the cursor that is placed second
	 * @param firstLabel name of the first reading, x0 or y0
	 * @param secondLabel name of the second reading
	 * @param differenceLabel name of the reading between them, dx or dy
	 * @param vertical true for X cursors, which put their text at the top and bottom of the line
	 * @param limits what the plot currently shows
	 */
	static void drawReadings(const Plot::Marker& first, const Plot::Marker& second, const char* firstLabel, const char* secondLabel, const char* differenceLabel, bool vertical, const ImPlotRect& limits)
	{
		const double gap = second.getValue() - first.getValue();

		/* A reading is text, so it is put next to the line it belongs to
		   rather than in the middle of the plot. Which side it goes on is
		   decided per line, and not once for the pair: a cursor sitting near
		   the far edge has to put its text on the inside or it runs off the
		   canvas, and the two cursors of a pair are often on opposite sides
		   of the middle. */
		const double step = 10.0 * GuiHelper::contentScale;
		const double lineHeight = 18.0 * GuiHelper::contentScale;

		if (vertical)
		{
			const auto sideOf = [&](const Plot::Marker& marker)
			{
				return marker.getValue() < (limits.X.Min + limits.X.Max) * 0.5 ? 1.0 : -1.0;
			};

			/* Both readings of a cursor share a side, so the second one is
			   placed below the first instead of on top of it. */
			ImPlot::Annotation(first.getValue(), limits.Y.Max, ImVec4(0, 0, 0, 0), ImVec2(static_cast<float>(sideOf(first) * step), 0.0f), true, "%s = %s", firstLabel, formatValue(first.getValue()).c_str());
			ImPlot::Annotation(second.getValue(), limits.Y.Min, ImVec4(0, 0, 0, 0), ImVec2(static_cast<float>(sideOf(second) * step), 0.0f), true, "%s = %s", secondLabel, formatValue(second.getValue()).c_str());
			/* The distance is what the pair is for, so it is drawn next to the
			   second line as well, below that line's own reading. */
			ImPlot::Annotation(second.getValue(), limits.Y.Min, ImVec4(0, 0, 0, 0), ImVec2(static_cast<float>(sideOf(second) * step), static_cast<float>(lineHeight)), true, "%s = %s", differenceLabel, formatValue(gap).c_str());
		}
		else
		{
			const auto sideOf = [&](const Plot::Marker& marker)
			{
				return marker.getValue() < (limits.Y.Min + limits.Y.Max) * 0.5 ? 1.0 : -1.0;
			};

			ImPlot::Annotation(limits.X.Min, first.getValue(), ImVec4(0, 0, 0, 0), ImVec2(0.0f, static_cast<float>(sideOf(first) * step)), true, "%s = %s", firstLabel, formatValue(first.getValue()).c_str());
			ImPlot::Annotation(limits.X.Max, second.getValue(), ImVec4(0, 0, 0, 0), ImVec2(0.0f, static_cast<float>(sideOf(second) * step)), true, "%s = %s", secondLabel, formatValue(second.getValue()).c_str());
			ImPlot::Annotation(limits.X.Max, second.getValue(), ImVec4(0, 0, 0, 0), ImVec2(0.0f, static_cast<float>(sideOf(second) * step - lineHeight)), true, "%s = %s", differenceLabel, formatValue(gap).c_str());
		}
	}

	/**
	 * @brief A value with the digits that carry no information taken off
	 *
	 * A cursor sitting on a whole number should not read as that number plus
	 * five zeroes, so the fixed point form is only used while there is
	 * something after the point to show.
	 */
	static std::string formatValue(double value)
	{
		char buffer[64];

		if (value == 0.0)
		{
			ImFormatString(buffer, sizeof(buffer), "0");
			return buffer;
		}

		if (std::abs(value) < 1e-4 || std::abs(value) >= 1e7)
			ImFormatString(buffer, sizeof(buffer), "%.5g", value);
		else
			ImFormatString(buffer, sizeof(buffer), "%.5f", value);

		std::string text = buffer;

		const size_t point = text.find('.');
		if (point != std::string::npos)
		{
			const size_t last = text.find_last_not_of('0');
			text.erase(last == std::string::npos ? point : (last > point ? last + 1 : point + 1));
		}

		return text;
	}

   private:
	/* The ids are chosen so that the two cursors of a direction never share
	   one, and so that a plot carrying both directions does not have an X
	   cursor and a Y cursor fighting over the same id. */
	static void drawX(Plot* plot, const ImPlotRect& limits, uint32_t idBase)
	{
		placeIfUntouched(plot->markerX0, limits.X.Min + (limits.X.Max - limits.X.Min) / 3.0);
		placeIfUntouched(plot->markerX1, limits.X.Min + 2.0 * (limits.X.Max - limits.X.Min) / 3.0);

		drawCrosshair(idBase + 0, plot->markerX0, true, xCursorColour);
		drawCrosshair(idBase + 1, plot->markerX1, true, ImVec4(xCursorColour.x, xCursorColour.y, xCursorColour.z, 0.7f));

		drawReadings(plot->markerX0, plot->markerX1, "x0", "x1", "dx", true, limits);
	}

	static void drawY(Plot* plot, const ImPlotRect& limits, uint32_t idBase)
	{
		placeIfUntouched(plot->markerY0, limits.Y.Min + (limits.Y.Max - limits.Y.Min) / 3.0);
		placeIfUntouched(plot->markerY1, limits.Y.Min + 2.0 * (limits.Y.Max - limits.Y.Min) / 3.0);

		drawCrosshair(idBase + 0, plot->markerY0, false, yCursorColour);
		drawCrosshair(idBase + 1, plot->markerY1, false, ImVec4(yCursorColour.x, yCursorColour.y, yCursorColour.z, 0.7f));

		drawReadings(plot->markerY0, plot->markerY1, "y0", "y1", "dy", false, limits);
	}

	/* A cursor is placed the first time it is drawn and left alone after
	   that, so that dragging one and then looking at another plot does not
	   send it back. The state flag is what distinguishes "never placed" from
	   "placed at the origin", which are different things: only the first is
	   moved here. */
	static void placeIfUntouched(Plot::Marker& marker, double place)
	{
		if (marker.getState())
			return;

		marker.setValue(place);
		marker.setState(true);
	}
};

#endif
