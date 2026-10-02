#ifndef _CURSORRENDERER_HPP
#define _CURSORRENDERER_HPP

#include <cmath>
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
	/* Every cursor is drawn in the same white. Directions are told apart by
	   running across the plot or up it, and the two lines of a direction by
	   their weight, so no colour is spent on saying either. */
	static constexpr ImVec4 cursorColour{1.0f, 1.0f, 1.0f, 1.0f};

	/* The two lines of a pair are drawn at different weights: the first of
	   them thin and the second thick. That is what tells them apart once
	   they are both white, and it also matches how the two read - the second
	   line is the one the span is measured to. */
	static constexpr float thinCursorWeight = 1.0f;
	static constexpr float thickCursorWeight = 2.0f;

	/**
	 * @brief Draws one cursor line and takes back the place it was dragged to
	 *
	 * @param id unique per plot, so that dragging one cursor does not move another
	 * @param marker the cursor being drawn
	 * @param vertical true for an X cursor, false for a Y cursor
	 * @param weight how thick to draw the line
	 * @param limits what the plot currently shows, which is how far a line reaches
	 */
	static void drawCrosshair(uint32_t id, Plot::Marker& marker, bool vertical, float weight, const ImPlotRect& limits)
	{
		/* The line is drawn from a copy, because ImPlot moves the value it is
		   given as the user drags, and the result has to be written back to
		   the marker or the cursor would return to where it was. */
		double position = marker.getValue();

		/* ImPlot is left to own the dragging, because it has the hit test,
		   the resize cursor and the write-back already, and none of that has
		   anything to do with how the line looks. Its own line is asked for
		   in a fully transparent colour so that only the behaviour is taken
		   from it: it can only draw a solid line, and these are dashed. A
		   colour is auto only when its alpha is -1, so a zero alpha here is
		   drawn as nothing rather than replaced by the text colour. */
		const ImVec4 invisibleLine(0.0f, 0.0f, 0.0f, 0.0f);

		if (vertical)
			ImPlot::DragLineX(static_cast<int>(id), &position, invisibleLine, weight);
		else
			ImPlot::DragLineY(static_cast<int>(id), &position, invisibleLine, weight);

		marker.setValue(position);

		drawDashedCrosshair(position, vertical, weight, limits);
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
	 * @param xSpan the distance between the X cursors, or zero when there are none
	 */
	static void drawY(Plot* plot, const ImPlotRect& limits, double xSpan = 0.0)
	{
		drawY(plot, limits, 2, xSpan);
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
	 * @param vertical true for X cursors, whose three readings run along the top of the plot
	 * @param limits what the plot currently shows
	 * @param run the width of the rectangle the pair closes with the other
	 *        direction, which is what turns it into a slope. Zero when the plot
	 *        carries no cursors along the other direction, and then the slope
	 *        has nothing to divide by and is not drawn
	 */
	static void drawReadings(const Plot::Marker& first, const Plot::Marker& second, const char* firstLabel, const char* secondLabel, const char* differenceLabel, bool vertical, const ImPlotRect& limits, double run = 0.0)
	{
		const double gap = second.getValue() - first.getValue();

		/* A reading is text, so it is put next to the line it belongs to
		   rather than in the middle of the plot. Which side it goes on is
		   the side away from the middle, because the middle of the plot is
		   where the data is: a reading pushed outward sits over the frame
		   and the empty margin instead of over the trace.

		   The side is decided per line rather than once for the pair. The
		   two lines of a pair are often on opposite sides of the middle,
		   and then both of their readings belong on the outside, which is
		   the far side for each of them in turn. Which line is the outer
		   one is a matter of where the user dragged them, so it is read off
		   the positions rather than assumed from the names. */
		const double step = 10.0 * GuiHelper::contentScale;
		const double lineHeight = 18.0 * GuiHelper::contentScale;

		if (vertical)
		{
			/* An X cursor left of the middle puts its text on its left,
			   and one right of the middle puts it on its right. */
			const auto sideOf = [&](const Plot::Marker& marker)
			{
				return Plot::readingOffsetSignAlongX(marker.getValue(), limits.X.Min, limits.X.Max);
			};

			/* The three of them belong to the pair, so they are stacked under
			   the second line: the second cursor's place, the distance to the
			   first one, and the rate that distance stands for. The stack
			   runs further out than the line itself, so it never turns back
			   over the trace; a line on the left stacks downward from the
			   top, and one on the right stacks upward from it. */
			ImPlot::Annotation(first.getValue(), limits.Y.Max, ImVec4(0, 0, 0, 0), ImVec2(static_cast<float>(sideOf(first) * step), 0.0f), true, "%s = %s", firstLabel, Plot::formatCursorMilliseconds(first.getValue()).c_str());

			ImPlot::Annotation(second.getValue(), limits.Y.Max, ImVec4(0, 0, 0, 0), ImVec2(static_cast<float>(sideOf(second) * step), 0.0f), true, "%s = %s", secondLabel, Plot::formatCursorMilliseconds(second.getValue()).c_str());
			ImPlot::Annotation(second.getValue(), limits.Y.Max, ImVec4(0, 0, 0, 0), ImVec2(static_cast<float>(sideOf(second) * step), static_cast<float>(sideOf(second) * lineHeight)), true, "%s = %s", differenceLabel, Plot::formatCursorMilliseconds(gap).c_str());
			ImPlot::Annotation(second.getValue(), limits.Y.Max, ImVec4(0, 0, 0, 0), ImVec2(static_cast<float>(sideOf(second) * step), static_cast<float>(sideOf(second) * 2.0 * lineHeight)), true, "1/dt = %s Hz", Plot::formatCursorRate(gap).c_str());
		}
		else
		{
			/* A Y cursor below the middle moves its text down, away from
			   the centre, and one above the middle moves it up. */
			const auto sideOf = [&](const Plot::Marker& marker)
			{
				return Plot::readingOffsetSignAlongY(marker.getValue(), limits.Y.Min, limits.Y.Max);
			};

			/* Both readings are written in one column against the right
			   edge of the plot, so that they line up with each other and
			   with the numbers on the vertical axis. A reading on the left
			   edge would sit among the axis ticks instead. Which of the two
			   cursors is the upper one is a matter of where the user
			   dragged them, so it is not assumed here: each reading carries
			   its own direction away from the middle, and that is what
			   keeps them apart. */
			const double readingColumn = limits.X.Max;

			ImPlot::Annotation(readingColumn, first.getValue(), ImVec4(0, 0, 0, 0), ImVec2(0.0f, static_cast<float>(sideOf(first) * step)), true, "%s = %s", firstLabel, formatValue(first.getValue()).c_str());
			ImPlot::Annotation(readingColumn, second.getValue(), ImVec4(0, 0, 0, 0), ImVec2(0.0f, static_cast<float>(sideOf(second) * step)), true, "%s = %s", secondLabel, formatValue(second.getValue()).c_str());
			ImPlot::Annotation(readingColumn, second.getValue(), ImVec4(0, 0, 0, 0), ImVec2(0.0f, static_cast<float>(sideOf(second) * (step + lineHeight))), true, "%s = %s", differenceLabel, formatValue(gap).c_str());

			/* A slope is only a number once there is a run to divide the
			   rise by, which is what the X cursors of the same plot give.
			   Without them the reading has nothing behind it, so the line is
			   left out rather than filled with a figure that means nothing. */
			if (run != 0.0)
				ImPlot::Annotation(readingColumn, second.getValue(), ImVec4(0, 0, 0, 0), ImVec2(0.0f, static_cast<float>(sideOf(second) * (step + 2.0 * lineHeight))), true, "dy/dx = %s", Plot::formatCursorSlope(gap, run).c_str());
		}
	}

	/**
	 * @brief A value with the digits that carry no information taken off
	 *
	 * The formatting is the plot's, because the same reading is written into
	 * the cursor label and into the text on the plot, and the two have to
	 * agree. This name is kept short because the reading calls below build
	 * their text from several of these at once.
	 */
	static std::string formatValue(double value)
	{
		return Plot::formatCursorValue(value);
	}

   private:
	/* How long a dash is and how long the space after it is, in the units the
	   drawing uses. The two are close enough that the pattern reads as a
	   broken line at a glance rather than as separate strokes. */
	static constexpr float dashLength = 6.0f;
	static constexpr float dashGap = 4.0f;

	/* One cursor line, broken into dashes.

	   The draw list only offers solid lines, so the dashes are laid out here.
	   They are measured from the first end of the line, so a line that is
	   dragged sideways keeps its pattern in the same place instead of the
	   dashes crawling along it frame by frame. */
	static void drawDashedCrosshair(double position, bool vertical, float weight, const ImPlotRect& limits)
	{
		ImVec2 from;
		ImVec2 to;

		if (vertical)
		{
			const float x = ImPlot::PlotToPixels(position, 0.0, IMPLOT_AUTO, IMPLOT_AUTO).x;

			from = ImVec2(x, ImPlot::PlotToPixels(0.0, limits.Y.Max, IMPLOT_AUTO, IMPLOT_AUTO).y);
			to = ImVec2(x, ImPlot::PlotToPixels(0.0, limits.Y.Min, IMPLOT_AUTO, IMPLOT_AUTO).y);
		}
		else
		{
			const float y = ImPlot::PlotToPixels(0.0, position, IMPLOT_AUTO, IMPLOT_AUTO).y;

			from = ImVec2(ImPlot::PlotToPixels(limits.X.Min, 0.0, IMPLOT_AUTO, IMPLOT_AUTO).x, y);
			to = ImVec2(ImPlot::PlotToPixels(limits.X.Max, 0.0, IMPLOT_AUTO, IMPLOT_AUTO).x, y);
		}

		const float dx = to.x - from.x;
		const float dy = to.y - from.y;
		const float length = std::sqrt(dx * dx + dy * dy);

		if (length <= 0.0f)
			return;

		const float unitX = dx / length;
		const float unitY = dy / length;
		const float stride = dashLength + dashGap;
		const ImU32 colour = ImGui::ColorConvertFloat4ToU32(cursorColour);
		ImDrawList* drawList = ImPlot::GetPlotDrawList();

		/* The dashes are clipped to the plot so that a cursor dragged to the
		   edge does not draw over the axis labels. */
		ImPlot::PushPlotClipRect();

		for (float start = 0.0f; start < length; start += stride)
		{
			const float remaining = length - start;
			const float stop = start + (remaining < dashLength ? remaining : dashLength);

			drawList->AddLine(ImVec2(from.x + unitX * start, from.y + unitY * start), ImVec2(from.x + unitX * stop, from.y + unitY * stop), colour, weight);
		}

		ImPlot::PopPlotClipRect();
	}

	/* The ids are chosen so that the two cursors of a direction never share
	   one, and so that a plot carrying both directions does not have an X
	   cursor and a Y cursor fighting over the same id. */
	static void drawX(Plot* plot, const ImPlotRect& limits, uint32_t idBase)
	{
		placeIfUntouched(plot->markerX0, limits.X.Min + (limits.X.Max - limits.X.Min) / 3.0);
		placeIfUntouched(plot->markerX1, limits.X.Min + 2.0 * (limits.X.Max - limits.X.Min) / 3.0);

		drawCrosshair(idBase + 0, plot->markerX0, true, thinCursorWeight * GuiHelper::contentScale, limits);
		drawCrosshair(idBase + 1, plot->markerX1, true, thickCursorWeight * GuiHelper::contentScale, limits);

		drawReadings(plot->markerX0, plot->markerX1, "x0", "x1", "dx", true, limits);
	}

	static void drawY(Plot* plot, const ImPlotRect& limits, uint32_t idBase, double run)
	{
		placeIfUntouched(plot->markerY0, limits.Y.Min + (limits.Y.Max - limits.Y.Min) / 3.0);
		placeIfUntouched(plot->markerY1, limits.Y.Min + 2.0 * (limits.Y.Max - limits.Y.Min) / 3.0);

		drawCrosshair(idBase + 0, plot->markerY0, false, thinCursorWeight * GuiHelper::contentScale, limits);
		drawCrosshair(idBase + 1, plot->markerY1, false, thickCursorWeight * GuiHelper::contentScale, limits);

		drawReadings(plot->markerY0, plot->markerY1, "y0", "y1", "dy", false, limits, run);
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
