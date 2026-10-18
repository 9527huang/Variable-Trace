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
#include "RtosEventRing.hpp"
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
		drawSourceLine(history, plotCursors);

		const double newest = history.newestSeconds();
		const RtosTimeline::Span window = windowOf(plotCursors, newest);

		std::vector<RtosTimeline::Sample> samples;
		history.copyRange(window.from, window.to, samples);

		if (samples.empty())
		{
			ImGui::TextDisabled("%s", I18n::tr("Nothing was published in this window."));

			return;
		}

		/*
		 * Runs, when the target keeps switch records, and samples when it does
		 * not.
		 *
		 * A run says a task held the processor from one moment to another; a
		 * sample says only that it was running when the round looked. The
		 * second cannot be drawn as a band without claiming the time between
		 * two rounds, so the shape follows the source rather than the
		 * preference.
		 */
		std::vector<RtosEventRing::Interval> runs;
		model.events().copyRange(window.from, window.to, runs);

		if (!runs.empty())
		{
			drawCounts(history, samples, true);

			drawGrid(snapshot, samples, runs, plotCursors, selectedTask, hoveredTask);
		}
		else
		{
			drawCounts(history, samples, false);

			drawGrid(snapshot, samples, {}, plotCursors, selectedTask, hoveredTask);
		}
	}

	/* Read and written by the panel, which keeps them with the rest of what the
	   reader set up. */
	static float getWindowSeconds()
	{
		return windowSeconds;
	}

	static void setWindowSeconds(float value)
	{
		windowSeconds = std::clamp(value, 1.0f, 600.0f);
	}

	static bool getFollowPlotCursors()
	{
		return followPlotCursors;
	}

	static void setFollowPlotCursors(bool value)
	{
		followPlotCursors = value;
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

	/*
	 * The window the reader has zoomed or panned to, if they have.
	 *
	 * Both of the automatic rules - the cursors in the plots, the last few
	 * seconds - decide the window for the reader. Once the wheel or a drag is
	 * used, the strip stops deciding: a window that jumped back to a rule while
	 * someone was looking through it would be worse than no zoom at all.
	 */
	inline static bool manualWindow = false;
	inline static double manualFrom = 0.0;
	inline static double manualTo = 0.0;

	/* The window rule lives in the timeline, where it has tests. */
	static RtosTimeline::Span windowOf(const std::vector<double>& plotCursors, double newest)
	{
		if (manualWindow)
			return {manualFrom, manualTo};

		return RtosTimeline::windowFrom(plotCursors, static_cast<double>(windowSeconds), followPlotCursors, newest);
	}

	/*
	 * The header is three rows, and it stays three rows: the controls, the
	 * legend, and what the window is showing.
	 *
	 * Everything used to be on one line, so at any width the reading gave out
	 * somewhere and the text on the right was quietly cut - "click a time to pin
	 * it" arrived as "click a time to pin it one mar...". A row that holds only
	 * controls has nothing to cut, and a row that holds only words can wrap.
	 */
	static void drawHeader(const RtosTimeline& history, const std::vector<double>& plotCursors)
	{
		ImGui::TextUnformatted(I18n::tr("Timeline"));

		ImGui::SameLine();
		ImGui::TextUnformatted(I18n::tr("last"));

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

		ImGui::SameLine();
		ImGui::TextUnformatted(I18n::tr("s"));

		ImGui::SameLine();
		ImGui::Checkbox(I18n::tr("follow plot cursors"), &followPlotCursors);

		/* Back to watching the run: the newest window, and no cursors deciding
		   it. The two are one action because "latest" is not where a cursor
		   placed an hour ago is pointing. */
		ImGui::SameLine();

		if (ImGui::Button(I18n::tr("back to latest")))
		{
			manualWindow = false;
			followPlotCursors = false;
		}

		if (hasPinned)
		{
			ImGui::SameLine();

			if (ImGui::Button(I18n::tr("unpin")))
				hasPinned = false;
		}
	}

	/*
	 * The third row: what the window is showing, and how to work it.
	 *
	 * Its own row rather than a suffix to the controls, because it is a sentence
	 * and sentences get cut when they share a line with buttons.
	 */
	static void drawSourceLine(const RtosTimeline& history, const std::vector<double>& plotCursors)
	{
		if (followPlotCursors && !plotCursors.empty() && !manualWindow)
		{
			ImGui::TextDisabled(I18n::tr("following %u plot cursor(s)"), static_cast<uint32_t>(plotCursors.size()));

			ImGui::SameLine();
		}

		/* What the marks are, and what they are not. A row of dots invites the
		   reading that a task ran between two of them, and the round interval is
		   what says how far apart they are. */
		const size_t kept = history.size();
		const double span = history.newestSeconds() - history.oldestSeconds();
		const double interval = kept > 1 && span > 0.0 ? span / static_cast<double>(kept - 1) : 0.0;

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

	/*
	 * What the window holds: how many rounds, how far apart, and how many
	 * places a round came back without a task.
	 *
	 * The last one is the reason this line exists. A strip of marks and blanks
	 * invites the reading that a blank is a task that was doing nothing, and the
	 * count is what says how much of the picture is missing rather than idle.
	 */
	static void drawCounts(const RtosTimeline& history, const std::vector<RtosTimeline::Sample>& samples, bool fromRuns)
	{
		const size_t kept = history.size();
		const double span = history.newestSeconds() - history.oldestSeconds();
		const double interval = kept > 1 && span > 0.0 ? span / static_cast<double>(kept - 1) : 0.0;

		size_t lanes = 0;
		size_t unread = 0;

		if (!samples.empty())
		{
			lanes = samples.back().entries.size();

			for (const RtosTimeline::Sample& sample : samples)
				unread += lanes > sample.entries.size() ? lanes - sample.entries.size() : 0u;
		}

		if (fromRuns)
		{
			/* Where the runs come from is worth saying: a reader who knows the
			   timeline is built from samples will read a band as a coarse
			   thing, and this one is not coarse. */
			ImGui::TextDisabled(I18n::tr("runs from the target's switch records, %.0f s held"), static_cast<double>(windowSeconds));
		}
		else if (interval > 0.0)
			ImGui::TextDisabled(I18n::tr("%u samples, %.2f s apart, %u unread"), static_cast<uint32_t>(samples.size()), interval,
			                    static_cast<uint32_t>(unread));
		else
			ImGui::TextDisabled(I18n::tr("%u samples, %u unread"), static_cast<uint32_t>(samples.size()), static_cast<uint32_t>(unread));

		if (unread > 0 && ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(I18n::tr("A round that could not read one of the kernel's lists leaves its tasks out of that sample. The gap is not a task that was idle."));
			ImGui::EndTooltip();
		}
	}

	static void drawGrid(const RtosSnapshot& snapshot, const std::vector<RtosTimeline::Sample>& samples,
	                     const std::vector<RtosEventRing::Interval>& runs, const std::vector<double>& plotCursors, uint32_t selectedTask,
	                     uint32_t hoveredTask)
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
		const float available = ImGui::GetContentRegionAvail().y;

		/* The strip takes two thirds of what is left under the header and the
		   table below it takes the rest. It used to be capped at a fixed
		   height, which was right in a column this strip shared with a table
		   and a second table, and leaves most of a window of its own empty. */
		const float height = std::clamp(available * 0.66f, 90.0f * scale, std::max(available - 2.0f, 90.0f * scale));
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
		   the mouse has to be given something to hit. It is also the thing that
		   takes focus, which is what lets the keyboard move the pin. */
		ImGui::InvisibleButton("##rtosTimelineGrid", ImVec2(width, height));

		const bool hovered = ImGui::IsItemHovered();
		const bool focused = ImGui::IsItemFocused();

		const auto timeAtX = [&](float x)
		{
			return oldest + std::clamp(static_cast<double>((x - plotLeft) / plotWidth), 0.0, 1.0) * span;
		};

		/*
		 * Zoom and pan, once the reader asks for them.
		 *
		 * The wheel zooms about the pointer, which is the one place the reader
		 * is looking; a drag moves the window under the hand. Both switch the
		 * automatic rules off, and both are undone by "back to latest".
		 */
		if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
		{
			const double at = timeAtX(ImGui::GetIO().MousePos.x);
			const double factor = ImGui::GetIO().MouseWheel > 0.0f ? 0.8 : 1.25;

			double from = oldest - (at - oldest) * (factor - 1.0);
			double to = newest + (newest - at) * (factor - 1.0);

			/* A window narrower than one round shows a single mark and no
			   gaps, and one wider than the ring shows what is not there. */
			if (to - from >= 0.5 && to - from <= 3600.0)
			{
				manualFrom = from;
				manualTo = to;
				manualWindow = true;
			}
		}

		if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f))
		{
			const double secondsPerPixel = span / static_cast<double>(plotWidth);

			manualFrom = oldest - ImGui::GetIO().MouseDelta.x * secondsPerPixel;
			manualTo = newest - ImGui::GetIO().MouseDelta.x * secondsPerPixel;
			manualWindow = true;
		}

		if (focused)
		{
			const double step = span * 0.05;

			if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
			{
				pinnedAt = hasPinned ? pinnedAt - step : oldest + span * 0.5;
				hasPinned = true;
			}

			if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
			{
				pinnedAt = hasPinned ? pinnedAt + step : oldest + span * 0.5;
				hasPinned = true;
			}

			if (ImGui::IsKeyPressed(ImGuiKey_Home))
			{
				manualWindow = false;
				followPlotCursors = false;
			}
		}

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

		/*
		 * The marks, when there are no runs.
		 *
		 * A mark is as wide as one row is tall, so a column of marks reads as a
		 * column and not as a bar. With runs drawn the marks would only repeat
		 * what the bands already say, less precisely.
		 */
		const float markSize = std::max(std::min(rowHeight - 2.0f * scale, 10.0f * scale), 3.0f * scale);

		for (const RtosTimeline::Sample& sample : runs.empty() ? samples : std::vector<RtosTimeline::Sample>{})
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
		 * The runs, as bands.
		 *
		 * One per switch: the task that was switched in, from then until the
		 * next switch. This is what the sampled marks could not say - a run has
		 * a length, a sample does not - and it is why the strip changes shape
		 * when the target reports switches.
		 */
		for (const RtosEventRing::Interval& run : runs)
		{
			for (size_t lane = 0; lane < lanes.size(); lane++)
			{
				if (lanes[lane] != run.tcbAddress)
					continue;

				const float from = timeToX(std::max(run.fromSeconds, oldest));
				const float to = timeToX(std::min(run.toSeconds, newest));

				if (to <= from)
					break;

				const float top = origin.y + static_cast<float>(lane) * rowHeight + 1.0f * scale;

				draw->AddRectFilled(ImVec2(from, top), ImVec2(to, top + rowHeight - 2.0f * scale), runningColour);

				break;
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

		/*
		 * The time axis, in ticks.
		 *
		 * Two labels at the ends leave the middle of a wide window without a
		 * scale, and a reader placing a mark in time needs one there. Five
		 * divisions are enough to read a moment to about a tenth of the window
		 * and few enough that the labels never run into each other.
		 */
		const int tickCount = 4;
		const ImU32 tickColour = ImGui::GetColorU32(ImGuiCol_TextDisabled);

		for (int tick = 0; tick <= tickCount; tick++)
		{
			const double at = oldest + span * tick / tickCount;
			const float x = timeToX(at);

			draw->AddLine(ImVec2(x, origin.y + plotHeight), ImVec2(x, origin.y + plotHeight + 3 * scale), tickColour);

			char label[32] = {};
			std::snprintf(label, sizeof(label), "%.1f", at);

			const float textWidth = ImGui::CalcTextSize(label).x;
			const float textX = tick == 0 ? x : (tick == tickCount ? x - textWidth : x - textWidth * 0.5f);

			draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.75f, ImVec2(textX, origin.y + plotHeight + 4 * scale), tickColour, label);
		}

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

		/* The caveat gets a line of its own, and that line wraps.
		 *
		 * It is the one thing about the strip that is easy to misread, so it
		 * stays on screen instead of moving into a tooltip; and it is a
		 * sentence, so it is allowed to take a second line rather than having
		 * its end cut off by the edge of the window. A row of legend entries
		 * joined by `SameLine` has no second line to take. */
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", I18n::tr("- the round did not read that task, which is not the same as idle"));
		ImGui::PopTextWrapPos();

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
