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
	static void draw(const RtosModel& model, const RtosSnapshot& snapshot)
	{
		const RtosTimeline& history = model.history();

		if (history.size() == 0)
		{
			ImGui::TextDisabled("%s", I18n::tr("The timeline fills up as rounds are published."));

			return;
		}

		drawHeader(history);

		const double newest = history.newestSeconds();
		const double from = newest - static_cast<double>(windowSeconds);

		std::vector<RtosTimeline::Sample> samples;
		history.copyRange(from, newest, samples);

		if (samples.empty())
		{
			ImGui::TextDisabled("%s", I18n::tr("Nothing was published in this window."));

			return;
		}

		drawGrid(snapshot, samples);
	}

   private:
	/* How much time the strip shows. Short enough that a round is a visible
	   mark, long enough to see a pattern. */
	inline static float windowSeconds = 10.0f;

	/* The moment the reader pinned, if they pinned one. */
	inline static bool hasPinned = false;
	inline static double pinnedAt = 0.0;

	static void drawHeader(const RtosTimeline& history)
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

		ImGui::SameLine();
		ImGui::TextDisabled("%s", I18n::tr("seconds back"));

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

	static void drawGrid(const RtosSnapshot& snapshot, const std::vector<RtosTimeline::Sample>& samples)
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

				drawState(draw, ImVec2(x - markSize * 0.5f, top), markSize, entry->state, scale);
			}
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
		ImGui::TextDisabled("%u %s   %u %s   %u %s   %u %s", running, toString(RtosTaskState::Running), ready, toString(RtosTaskState::Ready),
		                    blocked, toString(RtosTaskState::Blocked), suspended, toString(RtosTaskState::Suspended));

		if (unread > 0)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("   %u %s", unread, I18n::tr("not read"));
		}
	}

	/* The second encoding: every state has a shape as well as a hue. */
	static void drawState(ImDrawList* draw, const ImVec2& topLeft, float size, RtosTaskState state, float scale)
	{
		const ImVec2 bottomRight(topLeft.x + size, topLeft.y + size);

		switch (state)
		{
			case RtosTaskState::Running:
				draw->AddRectFilled(topLeft, bottomRight, runningColour);
				break;

			case RtosTaskState::Ready:
				draw->AddRect(topLeft, bottomRight, readyColour);
				break;

			case RtosTaskState::Blocked:
			{
				draw->AddRect(topLeft, bottomRight, blockedColour);

				/* One diagonal through the box is enough to tell it from an
				   outline at this size, and it survives being three pixels. */
				draw->AddLine(topLeft, bottomRight, blockedColour);
				break;
			}

			case RtosTaskState::Suspended:
			{
				draw->AddRect(topLeft, bottomRight, suspendedColour);

				const float dot = std::max(size * 0.28f, 1.0f * scale);

				draw->AddRectFilled(ImVec2(topLeft.x + dot, topLeft.y + size * 0.5f - dot * 0.5f),
				                    ImVec2(topLeft.x + dot + dot, topLeft.y + size * 0.5f + dot * 0.5f), suspendedColour);
				break;
			}

			default:
				/* No reading at that instant. Nothing is drawn, which is what
				   tells the reader the round did not see this task - not that
				   the task was idle. */
				break;
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
};

#endif
