#ifndef _GUIRTOSPANELTIMELINE_HPP
#define _GUIRTOSPANELTIMELINE_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "GuiHelper.hpp"
#include "I18n.hpp"
#include "RtosModel.hpp"
#include "RtosTableModel.hpp"
#include "RtosTimeline.hpp"
#include "imgui.h"

/*
 * The swimlanes under the task table.
 *
 * One row per task, one mark per round, the time across the bottom. What is
 * drawn is the samples the model kept, and nothing is joined up between them:
 * two rounds half a second apart say what was true at two moments and nothing
 * about the half second between them, so a mark is a mark and the gaps between
 * marks are gaps. A row with no mark at a column is a round that did not read
 * that task, which is a different thing from a task that was not running.
 *
 * Colour carries the state and only the state. Four hues are about as many as
 * anyone can tell apart, and there are fewer than four states to tell - but a
 * reader who cannot separate two hues still has to be able to read the row, so
 * every state also has its own shape: running is a filled block, ready is an
 * outline, blocked is hatched, suspended is dotted.
 *
 * Nothing is decided here. Which tasks have lanes and in what order comes from
 * `RtosTableModel::laneOrder`, which samples are in the window from the
 * timeline itself - both built into the test binary, which this file is not.
 */
class GuiRtosPanelTimeline
{
   public:
	static void draw(const RtosModel& model, const RtosSnapshot& snapshot, const std::vector<double>& plotCursors, uint32_t selectedTask, uint32_t hoveredTask)
	{
		const RtosTimeline& history = model.history();

		if (history.size() == 0)
		{
			ImGui::TextDisabled("%s", I18n::tr("The timeline fills up as rounds are published."));

			return;
		}

		drawHeader(history, plotCursors);
		drawLegend();

		const double newest = history.newestSeconds();
		const RtosTimeline::Span window = windowOf(plotCursors, newest);

		std::vector<RtosTimeline::Sample> samples;
		history.copyRange(window.from, window.to, samples);

		if (samples.empty())
		{
			ImGui::TextDisabled("%s", I18n::tr("Nothing was published in this window."));

			return;
		}

		drawGrid(snapshot, samples, plotCursors, selectedTask, hoveredTask);
	}

   private:
	/* How much time the strip shows. Short enough that a round is a visible
	   mark, long enough to see a pattern. */
	inline static float windowSeconds = 10.0f;

	/* The moment the reader pinned, if they pinned one. */
	inline static bool hasPinned = false;
	inline static double pinnedAt = 0.0;

	/* Whether the strip follows the cursors placed in the variable view. On,
	 * because that is what the two views sharing a clock is for; off, because a
	 * reader looking at an old measurement still wants to watch the live rounds.
	 */
	inline static bool followPlotCursors = true;

	/* The window rule lives in the timeline, where it has tests. */
	static RtosTimeline::Span windowOf(const std::vector<double>& plotCursors, double newest)
	{
		return RtosTimeline::windowFrom(plotCursors, static_cast<double>(windowSeconds), followPlotCursors, newest);
	}

	static void drawHeader(const RtosTimeline& history, const std::vector<double>& plotCursors)
	{
		ImGui::TextUnformatted(I18n::tr("Timeline"));

		ImGui::SameLine();
		ImGui::SetNextItemWidth(70 * GuiHelper::contentScale);

		if (ImGui::InputFloat("##rtosTimelineWindow", &windowSeconds, 0.0f, 0.0f, "%.0f"))
			windowSeconds = std::clamp(windowSeconds, 1.0f, 600.0f);

		if (ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(I18n::tr("How many seconds of history to show."));
			ImGui::EndTooltip();
		}

		if (followPlotCursors && !plotCursors.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled(I18n::tr("following %u plot cursor(s)"), static_cast<uint32_t>(plotCursors.size()));
		}

		ImGui::SameLine();
		ImGui::Checkbox(I18n::tr("follow plot cursors"), &followPlotCursors);

		/* What the marks are, and what they are not. A row of dots invites the
		   reading that a task ran between two of them, and the round interval is
		   what says how far apart they are. */
		const size_t kept = history.size();
		const double span = history.newestSeconds() - history.oldestSeconds();
		const double interval = kept > 1 && span > 0.0 ? span / static_cast<double>(kept - 1) : 0.0;

		ImGui::SameLine();

		if (!hasPinned)
		{
			ImGui::TextDisabled("%s", I18n::tr("click a time to pin it"));
			ImGui::SameLine();
		}

		if (interval > 0.0)
			ImGui::TextDisabled(I18n::tr("one mark per round, %.2f s apart"), interval);
		else
			ImGui::TextDisabled("%s", I18n::tr("one mark per round"));

		ImGui::Separator();
	}

	static void drawGrid(const RtosSnapshot& snapshot, const std::vector<RtosTimeline::Sample>& samples, const std::vector<double>& plotCursors,
	                     uint32_t selectedTask, uint32_t hoveredTask)
	{
		const std::vector<uint32_t> lanes = RtosTableModel::laneOrder(snapshot);

		if (lanes.empty())
		{
			ImGui::TextDisabled("%s", I18n::tr("No tasks in this round."));

			return;
		}

		const float scale = GuiHelper::contentScale;
		const float nameWidth = 110.0f * scale;
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float width = ImGui::GetContentRegionAvail().x;
		const float height = std::clamp(ImGui::GetContentRegionAvail().y * 0.9f, 90.0f * scale, 220.0f * scale);
		const float axisHeight = 16.0f * scale;
		const float plotHeight = height - axisHeight;

		const float rowHeight = plotHeight / static_cast<float>(lanes.size());
		const float plotLeft = origin.x + nameWidth;
		const float plotWidth = std::max(width - nameWidth, 40.0f * scale);

		const double newest = samples.back().atSeconds;
		const double oldest = samples.front().atSeconds;
		const double span = std::max(newest - oldest, 1e-6);

		const auto timeToX = [&](double atSeconds)
		{
			return plotLeft + static_cast<float>((atSeconds - oldest) / span) * plotWidth;
		};

		/* One button over the whole strip: the grid is drawn, not laid out, so
		   the mouse has to be given something to hit. */
		ImGui::InvisibleButton("##rtosTimelineGrid", ImVec2(width, height));

		const bool hovered = ImGui::IsItemHovered();

		ImDrawList* draw = ImGui::GetWindowDrawList();

		/* The frame and the lane labels. */
		draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + plotHeight), ImGui::GetColorU32(ImGuiCol_FrameBg, 0.35f));
		draw->AddRect(origin, ImVec2(origin.x + width, origin.y + plotHeight), ImGui::GetColorU32(ImGuiCol_Border));

		for (size_t lane = 0; lane < lanes.size(); lane++)
		{
			const float top = origin.y + static_cast<float>(lane) * rowHeight;

			if (lane % 2 == 1)
				draw->AddRectFilled(ImVec2(plotLeft, top), ImVec2(plotLeft + plotWidth, top + rowHeight), ImGui::GetColorU32(ImGuiCol_Text, 0.03f));

			const RtosTask* task = snapshot.find(lanes[lane]);

			if (task != nullptr && !task->name.empty())
			{
				draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f, ImVec2(origin.x + 4 * scale, top + rowHeight * 0.1f),
				              ImGui::GetColorU32(ImGuiCol_Text), task->name.c_str(), nullptr, plotLeft - origin.x - 6 * scale);
			}
		}

		/*
		 * The lane the reader is on, in the table above.
		 *
		 * Drawn under the marks so that the marks stay readable, and only for a
		 * task the table is pointing at: a highlight that appears for every
		 * row the mouse crosses would be a flicker, not a pointer.
		 */
		const auto highlightLane = [&](uint32_t address, ImU32 colour)
		{
			if (address == 0)
				return;

			for (size_t lane = 0; lane < lanes.size(); lane++)
			{
				if (lanes[lane] != address)
					continue;

				const float top = origin.y + static_cast<float>(lane) * rowHeight;

				draw->AddRectFilled(ImVec2(origin.x, top), ImVec2(plotLeft + plotWidth, top + rowHeight), colour);
			}
		};

		highlightLane(hoveredTask, ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.22f));
		highlightLane(selectedTask, ImGui::GetColorU32(ImGuiCol_Header, 0.35f));

		/* The marks. A mark is as wide as one row is tall, so a column of marks
		   reads as a column and not as a bar. */
		const float markSize = std::max(std::min(rowHeight - 2.0f * scale, 10.0f * scale), 3.0f * scale);

		for (const RtosTimeline::Sample& sample : samples)
		{
			const float x = timeToX(sample.atSeconds);

			if (x < plotLeft - 1.0f || x > plotLeft + plotWidth + 1.0f)
				continue;

			for (size_t lane = 0; lane < lanes.size(); lane++)
			{
				const auto entry = std::find_if(sample.entries.begin(), sample.entries.end(),
				                                [&](const RtosTimeline::Entry& candidate) { return candidate.tcbAddress == lanes[lane]; });

				if (entry == sample.entries.end())
					continue;

				const float top = origin.y + static_cast<float>(lane) * rowHeight + (rowHeight - markSize) * 0.5f;

				if (hasMark(entry->state))
					drawMark(draw, ImVec2(x - markSize * 0.5f, top), markSize, markOf(entry->state), scale);
			}
		}

		/*
		 * Where the variable view's cursors are.
		 *
		 * Thin, and labelled on the axis, because they are not this view's own
		 * marks: the reader placed them in a plot and is watching them move
		 * there. The pin this view keeps for reading a moment out is the thick
		 * one, so the two are never confused.
		 */
		for (const double at : plotCursors)
		{
			if (at < oldest || at > newest)
				continue;

			const float x = timeToX(at);

			draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + plotHeight), plotCursorColour, 1.0f);

			/* The label goes under the axis, and is left off where it would sit
			   on top of the axis's own last label: two times printed over each
			   other read as one wrong time. */
			const float labelX = x + 2.0f * scale;

			if (labelX < plotLeft + plotWidth - 54 * scale)
				draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.75f, ImVec2(labelX, origin.y + plotHeight + 2 * scale), plotCursorColour,
				              formatSeconds(at).c_str());
		}

		/* The time axis: two labels and the newest time, because a strip of dots
		   without a scale says nothing about when anything happened. */
		draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.8f, ImVec2(plotLeft, origin.y + plotHeight + 2 * scale),
		              ImGui::GetColorU32(ImGuiCol_TextDisabled), formatSeconds(oldest).c_str());
		draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.8f,
		              ImVec2(plotLeft + plotWidth - 60 * scale, origin.y + plotHeight + 2 * scale),
		              ImGui::GetColorU32(ImGuiCol_TextDisabled), formatSeconds(newest).c_str());

		drawCursor(draw, samples, lanes, origin, rowHeight, plotLeft, plotWidth, hovered);
	}

	static void drawCursor(ImDrawList* draw, const std::vector<RtosTimeline::Sample>& samples, const std::vector<uint32_t>& lanes, const ImVec2& origin,
	                       float rowHeight, float plotLeft, float plotWidth, bool hovered)
	{
		ImGuiIO& io = ImGui::GetIO();

		/* A click pins the moment the mouse is over, and a second click on the
		   same strip moves it. Nothing is pinned until someone asks, so the
		   strip shows the whole window by default. */
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			const double oldest = samples.front().atSeconds;
			const double newest = samples.back().atSeconds;
			const double share = std::clamp(static_cast<double>((io.MousePos.x - plotLeft) / plotWidth), 0.0, 1.0);

			pinnedAt = oldest + (newest - oldest) * share;
			hasPinned = true;
		}

		const double newest = samples.back().atSeconds;
		const double oldest = samples.front().atSeconds;
		const double span = std::max(newest - oldest, 1e-6);

		const double at = hasPinned ? pinnedAt : (hovered ? oldest + std::clamp(static_cast<double>((io.MousePos.x - plotLeft) / plotWidth), 0.0, 1.0) * (newest - oldest) : 0.0);

		if (!hasPinned && !hovered)
			return;

		const float x = plotLeft + static_cast<float>((at - oldest) / span) * plotWidth;

		draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + rowHeight * static_cast<float>(lanes.size())), ImGui::GetColorU32(ImGuiCol_Text, 0.55f));

		const size_t nearest = RtosTimeline::nearestIndex(samples, at);

		if (nearest >= samples.size())
			return;

		const RtosTimeline::Sample& sample = samples[nearest];

		/* What the moment held. Counted by state, and the tasks that were not
		   idle are listed, because which one was running is the question this
		   strip is here to answer. */
		uint32_t running = 0;
		uint32_t ready = 0;
		uint32_t blocked = 0;
		uint32_t suspended = 0;
		uint32_t unread = 0;

		for (const uint32_t lane : lanes)
		{
			const auto entry = std::find_if(sample.entries.begin(), sample.entries.end(),
			                                [&](const RtosTimeline::Entry& candidate) { return candidate.tcbAddress == lane; });

			if (entry == sample.entries.end())
			{
				unread++;
				continue;
			}

			switch (entry->state)
			{
				case RtosTaskState::Running:
					running++;
					break;

				case RtosTaskState::Ready:
					ready++;
					break;

				case RtosTaskState::Blocked:
					blocked++;
					break;

				case RtosTaskState::Suspended:
					suspended++;
					break;

				default:
					unread++;
					break;
			}
		}

		ImGui::Text("%s %.2f s", I18n::tr("at"), at);
		ImGui::SameLine();
		ImGui::TextDisabled("%u %s   %u %s   %u %s   %u %s", running, RtosTableModel::taskStateLabel(RtosTaskState::Running), ready,
		                    RtosTableModel::taskStateLabel(RtosTaskState::Ready), blocked, RtosTableModel::taskStateLabel(RtosTaskState::Blocked),
		                    suspended, RtosTableModel::taskStateLabel(RtosTaskState::Suspended));

		if (unread > 0)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("   %u %s", unread, I18n::tr("not read"));
		}
	}

	/*
	 * How a state looks, in one place.
	 *
	 * The legend and the grid read this together, so a change to one cannot
	 * drift from the other: a legend that explains marks the grid no longer
	 * draws is worse than no legend.
	 */
	struct StateMark
	{
		enum class Shape
		{
			Filled,
			Outline,
			Hatched,
			Dotted,
		};

		ImU32 colour = 0;
		Shape shape = Shape::Outline;
	};

	/* A round that did not read a task has no state, and a mark for it would
	   say the task was idle. Nothing is drawn there, and the legend says so. */
	static bool hasMark(RtosTaskState state)
	{
		return state != RtosTaskState::Unknown;
	}

	static StateMark markOf(RtosTaskState state)
	{
		switch (state)
		{
			case RtosTaskState::Running:
				return {runningColour, StateMark::Shape::Filled};

			case RtosTaskState::Blocked:
				return {blockedColour, StateMark::Shape::Hatched};

			case RtosTaskState::Suspended:
				return {suspendedColour, StateMark::Shape::Dotted};

			case RtosTaskState::Ready:
			case RtosTaskState::PendingReady:
			case RtosTaskState::Deleted:
			case RtosTaskState::Unknown:
			default:
				/* Ready also covers a task that is ready with the scheduler
				   suspended, and one whose list it was found on is the only
				   thing that says so. Both are an outline. */
				return {readyColour, StateMark::Shape::Outline};
		}
	}

	/* The second encoding: every state has a shape as well as a hue. */
	static void drawMark(ImDrawList* draw, const ImVec2& topLeft, float size, const StateMark& mark, float scale)
	{
		const ImVec2 bottomRight(topLeft.x + size, topLeft.y + size);

		switch (mark.shape)
		{
			case StateMark::Shape::Filled:
				draw->AddRectFilled(topLeft, bottomRight, mark.colour);
				break;

			case StateMark::Shape::Outline:
				draw->AddRect(topLeft, bottomRight, mark.colour);
				break;

			case StateMark::Shape::Hatched:
				draw->AddRect(topLeft, bottomRight, mark.colour);

				/* One diagonal through the box is enough to tell it from an
				   outline at this size, and it survives being three pixels. */
				draw->AddLine(topLeft, bottomRight, mark.colour);
				break;

			case StateMark::Shape::Dotted:
			{
				draw->AddRect(topLeft, bottomRight, mark.colour);

				const float dot = std::max(size * 0.28f, 1.0f * scale);

				draw->AddRectFilled(ImVec2(topLeft.x + dot, topLeft.y + size * 0.5f - dot * 0.5f),
				                    ImVec2(topLeft.x + dot + dot, topLeft.y + size * 0.5f + dot * 0.5f), mark.colour);
				break;
			}
		}
	}

	/*
	 * What the marks mean, always on screen.
	 *
	 * The shapes were already there; a reader meeting them for the first time
	 * had no way to find out what they meant without being told. The blank case
	 * is part of the legend because it is the one that is easiest to misread:
	 * nothing drawn means the round did not read the task, which is not the same
	 * as the task having been idle.
	 */
	static void drawLegend()
	{
		const float scale = GuiHelper::contentScale;
		const float size = 8.0f * scale;
		const float spacing = 18.0f * scale;

		const RtosTaskState states[] = {RtosTaskState::Running, RtosTaskState::Ready, RtosTaskState::Blocked, RtosTaskState::Suspended};

		ImDrawList* draw = ImGui::GetWindowDrawList();

		for (const RtosTaskState state : states)
		{
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const ImVec2 topLeft(origin.x, origin.y + (ImGui::GetTextLineHeight() - size) * 0.5f);

			drawMark(draw, topLeft, size, markOf(state), scale);

			ImGui::Dummy(ImVec2(size + 4.0f * scale, 0.0f));
			ImGui::SameLine(0.0f, 4.0f * scale);
			ImGui::TextUnformatted(RtosTableModel::taskStateLabel(state));

			ImGui::SameLine(0.0f, spacing);
		}

		/* The blank, drawn as an empty slot so that the legend has a hole where
		   the grid has one. */
		{
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const ImVec2 topLeft(origin.x, origin.y + (ImGui::GetTextLineHeight() - size) * 0.5f);

			draw->AddRect(topLeft, ImVec2(topLeft.x + size, topLeft.y + size), ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.5f));

			ImGui::Dummy(ImVec2(size + 4.0f * scale, 0.0f));
			ImGui::SameLine(0.0f, 4.0f * scale);
			ImGui::TextDisabled("%s", I18n::tr("blank"));
		}

		ImGui::SameLine(0.0f, spacing);
		ImGui::TextDisabled("%s", I18n::tr("thin line"));
		ImGui::SameLine(0.0f, 4.0f * scale);
		ImGui::TextDisabled("%s", I18n::tr("a cursor in the variable view"));

		ImGui::SameLine(0.0f, spacing);
		ImGui::TextDisabled("%s", I18n::tr("- the round did not read that task, which is not the same as idle"));

		if (ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(I18n::tr("One mark is one round, not an interval: what was true at that instant, and nothing about the time between two of them."));
			ImGui::EndTooltip();
		}
	}

	static std::string formatSeconds(double atSeconds)
	{
		char buffer[32] = {};
		std::snprintf(buffer, sizeof(buffer), "%.1f s", atSeconds);

		return buffer;
	}

	/* Fixed hues rather than theme colours: these four have to stay apart from
	   one another, and a theme's accent colour is chosen for something else.
	   They are picked to read on a light background and a dark one. */
	inline static const ImU32 runningColour = IM_COL32(60, 160, 90, 255);
	inline static const ImU32 readyColour = IM_COL32(70, 130, 210, 255);
	inline static const ImU32 blockedColour = IM_COL32(210, 140, 55, 255);
	inline static const ImU32 suspendedColour = IM_COL32(140, 140, 145, 255);

	/* The variable view's cursors: a hue of their own, so that a thin line is
	   never mistaken for one of the state marks. */
	inline static const ImU32 plotCursorColour = IM_COL32(110, 175, 200, 220);
};

#endif
