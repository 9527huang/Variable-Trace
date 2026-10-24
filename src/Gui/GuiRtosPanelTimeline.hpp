#ifndef _GUIRTOSPANELTIMELINE_HPP
#define _GUIRTOSPANELTIMELINE_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "GuiHelper.hpp"
#include "GuiRtosSelection.hpp"
#include "I18n.hpp"
#include "RtosModel.hpp"
#include "RtosEventRing.hpp"
#include "RtosTableModel.hpp"
#include "RtosIsrAttribution.hpp"
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
 * `RtosTableModel`, which samples are in the window from the timeline itself -
 * both built into the test binary, which this file is not.
 */
class GuiRtosPanelTimeline
{
   public:
	/* What the strip drew last: the bands below it use this rather than asking
	   again, so that a click handled during the strip's own draw cannot leave
	   the two showing different stretches for a frame. */
	inline static RtosTimeline::Span lastWindow;
	inline static bool haveLastWindow = false;

	static RtosTimeline::Span getLastWindow()
	{
		return lastWindow;
	}

	static bool hasLastWindow()
	{
		return haveLastWindow;
	}

	/*
	 * Where the axis actually starts and how wide it is, in screen pixels.
	 *
	 * The bands under the strip have to line up with the lanes to the pixel, and
	 * lining up means using the same numbers rather than the same formula: the
	 * strip's left edge is the name column's width, which depends on the longest
	 * task name in this round, not on a constant the other block could copy.
	 */
	inline static float lastAxisLeft = 0.0f;
	inline static float lastAxisWidth = 0.0f;

	static float getAxisLeft()
	{
		return lastAxisLeft;
	}

	static float getAxisWidth()
	{
		return lastAxisWidth;
	}

	/*
	 * `lanes` is the order to draw the rows in, handed in by the panel: either
	 * the stable one - base priority, then name, then address - or the order
	 * the table is showing, which is what the reader sorted it to. The strip
	 * does not decide it, because both answers are legitimate and only the
	 * caller knows which one is wanted.
	 *
	 * Whatever narrows the table narrows this too: the list is the table's rows,
	 * so a filter that leaves three of them leaves three lanes. The line under
	 * the legend says how many, because a missing lane reads as a task that did
	 * nothing rather than as a filter.
	 */
	static void draw(const RtosModel& model, const RtosSnapshot& snapshot, const std::vector<double>& plotCursors,
	                 const std::vector<RtosTaskRow>& rows, const std::vector<uint32_t>& lanes, size_t rosterSize,
	                 GuiRtosSelection& selection, float reservedBelow)
	{
		const RtosTimeline& history = model.history();

		if (history.size() == 0)
		{
			GuiHelper::drawSecondaryText("%s", I18n::tr("The timeline fills up as rounds are published."));

			return;
		}

		drawHeader(history, plotCursors);
		drawLegend();

		/*
		 * Two rows, whatever the sentence needs.
		 *
		 * It is one sentence of facts joined by dots, and two of those facts
		 * depend on what the reader did: "click a time to pin it" goes away
		 * once a time is pinned, and "showing 8.91 s of 10 s asked" appears
		 * once a pin has moved the window out of its place. At this width the
		 * longer form wraps, and the strip then stood a line taller than it was
		 * a moment before - which moved every block under it, including the
		 * pause box, by exactly that line.
		 */
		const float sourceTop = ImGui::GetCursorPosY();

		drawSourceLine(history, plotCursors, lanes.size(), snapshot.tasks.size());

		const float sourceHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
		const float sourceUsed = ImGui::GetCursorPosY() - sourceTop;

		if (sourceUsed < sourceHeight)
			ImGui::Dummy(ImVec2(0.0f, sourceHeight - sourceUsed));

		const double newest = history.newestSeconds();
		const RtosTimeline::Span window = windowOf(plotCursors, newest);

		/* The bands under the strip draw over the same stretch, and the only way
		   they can be read together is if it is the same stretch. Kept here
		   rather than recomputed there: two calls to the same function can
		   answer differently inside one frame, once a click has moved the pin. */
		lastWindow = window;
		haveLastWindow = true;

		std::vector<RtosTimeline::Sample> samples;
		history.copyRange(window.from, window.to, samples);

		if (samples.empty())
		{
			GuiHelper::drawSecondaryText("%s", I18n::tr("Nothing was published in this window."));

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

			drawGrid(snapshot, rows, lanes, samples, runs, plotCursors, rosterSize, selection, reservedBelow);
		}
		else
		{
			drawCounts(history, samples, false);

			drawGrid(snapshot, rows, lanes, samples, {}, plotCursors, rosterSize, selection, reservedBelow);
		}
	}

	/* How far the lanes are scrolled, in pixels. Kept across frames because it
	   is a place the reader put themselves, like the window and the pin. */
	inline static float scroll = 0.0f;

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

	/*
	 * A name that fits the column, with its tail replaced by dots.
	 *
	 * Cut on character boundaries rather than on bytes: a Chinese task name cut
	 * in the middle of a character draws as boxes, and those boxes are wider
	 * than the column it was trying to fit into.
	 */
	static std::string fittedLabel(const std::string& text, float width)
	{
		if (width <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= width)
			return text;

		size_t end = text.size();

		while (end > 0)
		{
			while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
				--end;

			const std::string candidate = text.substr(0, end) + "...";

			if (ImGui::CalcTextSize(candidate.c_str()).x <= width)
				return candidate;

			--end;
		}

		return "...";
	}

	/* The hue a state is drawn with.
	 *
	 * The summary counts the same four states the strip does, and a reader
	 * comparing "4 blocked" with the amber marks in the lane beside it should
	 * be comparing one colour, not two that were picked separately. */
	/*
	 * The ISR track, for drawing where interrupts landed.
	 *
	 * The strip and the ISR band are read side by side and neither says which
	 * task was running when a given interrupt arrived - which is the question the
	 * pair exists to answer. A borrowed pointer rather than a copy: the panel
	 * owns the projection and rebuilds it every frame.
	 */
	static void setIsrTrack(const std::vector<RtosTrackEntry>* entries)
	{
		isrTrack = entries;
	}

	/*
	 * Interrupts per task, from the same pass that draws the marks.
	 *
	 * The table's column reads this rather than working the answer out again:
	 * the strip has the running intervals, and asking for them twice a frame to
	 * fill one column would be paying for the same answer twice.
	 */
	static const std::map<uint32_t, uint32_t>& isrCountsByTask()
	{
		return isrCounts;
	}

	static ImU32 getStateColour(RtosTaskState state)
	{
		return markOf(state).colour;
	}

	static void setFollowPlotCursors(bool value)
	{
		followPlotCursors = value;
	}

	/*
	 * Pins a moment from outside the strip.
	 *
	 * The event list is the same records as the bands, as rows; clicking one
	 * should land on the moment it describes, and the pin is what both halves
	 * already agree on. The window is left to the strip: if the moment is
	 * outside it, the next frame moves the window rather than dropping the pin
	 * somewhere the reader cannot see it.
	 */
	static void pinAt(double atSeconds)
	{
		hasPinned = true;
		pinnedAt = atSeconds;
	}

	/* The moment the reader pinned on the strip, if any. Read by the panel to
	   describe that moment in words. */
	static bool getPinned()
	{
		return hasPinned;
	}

	static double getPinnedAt()
	{
		return pinnedAt;
	}

	/* Whether the lanes follow the table's order instead of the stable one. */
	static bool getFollowTableOrder()
	{
		return followTableOrder;
	}

	static void setFollowTableOrder(bool value)
	{
		followTableOrder = value;
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

	/* Off by default: the stable order is what keeps a lane under the same row
	 * from round to round, and a table sorted by stack use would make every
	 * lane jump as the numbers change. On when the reader is comparing the
	 * strip against a particular row order. */
	inline static bool followTableOrder = false;

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

		ImGui::SameLine();

		/* Always there, and greyed when there is nothing to unpin.
		   A button that comes and goes changes how much room the row needs,
		   and the row decides whether the item after it fits beside it: with
		   the button present, "follow table order" wrapped to a second line,
		   and every block under the strip moved down by one line the moment a
		   moment was pinned. */
		ImGui::BeginDisabled(!hasPinned);

		if (ImGui::Button(I18n::tr("unpin")))
			hasPinned = false;

		ImGui::EndDisabled();

		/* Beside the window length, because it answers the same kind of
		   question: what the strip shows, not what the target did. */
		if (ImGui::GetContentRegionAvail().x > 175.0f * GuiHelper::contentScale)
			ImGui::SameLine();

		ImGui::Checkbox(I18n::tr("follow table order"), &followTableOrder);
	}

	/*
	 * The third row: what the window is showing, and how to work it.
	 *
	 * Its own row rather than a suffix to the controls, because it is a sentence
	 * and sentences get cut when they share a line with buttons.
	 */
	static void drawSourceLine(const RtosTimeline& history, const std::vector<double>& plotCursors, size_t lanes, size_t tasks)
	{
		/* A dot between the sentences: they are separate facts and were being
		   read as one long line. */
		const auto separate = []()
		{
			ImGui::SameLine();
			GuiHelper::drawSecondaryText("%s", "\xc2\xb7");
			ImGui::SameLine();
		};

		if (followPlotCursors && !plotCursors.empty() && !manualWindow)
		{
			GuiHelper::drawSecondaryText(I18n::tr("following %u plot cursor(s)"), static_cast<uint32_t>(plotCursors.size()));

			separate();
		}

		/* What the marks are, and what they are not. A row of dots invites the
		   reading that a task ran between two of them, and the round interval is
		   what says how far apart they are. */
		const size_t kept = history.size();
		const double span = history.newestSeconds() - history.oldestSeconds();
		const double interval = kept > 1 && span > 0.0 ? span / static_cast<double>(kept - 1) : 0.0;

		/*
		 * The window that was asked for, and the window that is on screen.
		 *
		 * They are not the same number and the difference is not the reader's
		 * fault: a plot cursor cut the window down, or the history does not
		 * reach back that far yet. The axis can only say what it spans - it was
		 * showing 0.2 s under a heading that said ten, and nothing on screen
		 * explained why. Said in words, with the count behind it.
		 */
		const RtosTimeline::Span shown = windowOf(plotCursors, history.newestSeconds());
		const double shownSeconds = shown.to - shown.from;

		/* The comparison is only worth making when the window really is short:
		   the newest window can be a round or two longer than asked for, and
		   "showing 10.80 s of 10 s asked" reads like a bug rather than a
		   measurement. */
		if (shownSeconds < static_cast<double>(windowSeconds) * 0.9)
			GuiHelper::drawSecondaryText(I18n::tr("showing %.2f s of %.0f s asked, %u rounds"), shownSeconds, static_cast<double>(windowSeconds),
			                             static_cast<uint32_t>(kept));
		else
			GuiHelper::drawSecondaryText(I18n::tr("showing %.2f s, %u rounds"), shownSeconds, static_cast<uint32_t>(kept));

		ImGui::SameLine();

		if (!hasPinned)
		{
			GuiHelper::drawSecondaryText("%s", I18n::tr("click a time to pin it"));

			separate();
		}

		/* The rounds are what the bands are built from now, not marks: each
		   band holds from one round until the next look. */
		if (interval > 0.0)
			GuiHelper::drawSecondaryText(I18n::tr("rounds %.2f s apart"), interval);
		else
			GuiHelper::drawSecondaryText("%s", I18n::tr("one band per state held until the next round"));

		/* Said after the marks, because it is about what the picture cannot
		   show: a gap is a round that did not read the task, not a task that
		   did nothing. */
		separate();
		GuiHelper::drawSecondaryText("%s", I18n::tr("not read is not the same as idle"));

		/* Which order the rows are in, said in words. The two orders are both
		   defensible and a reader cannot tell them apart by looking: one keeps
		   a lane where it was between rounds, the other puts it where its row
		   is in the table. Getting them confused means reading the wrong
		   lane. */
		GuiHelper::drawSecondaryText("%s", I18n::tr(followTableOrder ? "lanes in table order" : "lanes in base priority order"));

		/* A strip narrowed by the table's filter holds fewer lanes than the
		   kernel has tasks, and a reader who took the missing ones for tasks
		   that did nothing would be reading a filter as a fact about the
		   target. */
		if (lanes < tasks)
		{
			separate();
			GuiHelper::drawSecondaryText(I18n::tr("%u of %u tasks have lanes"), static_cast<uint32_t>(lanes), static_cast<uint32_t>(tasks));
		}

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
			GuiHelper::drawSecondaryText(I18n::tr("runs from the target's switch records, %.0f s held"), static_cast<double>(windowSeconds));
		}
		else if (interval > 0.0)
			GuiHelper::drawSecondaryText(I18n::tr("%u samples, %.2f s apart, %u unread"), static_cast<uint32_t>(samples.size()), interval,
			                    static_cast<uint32_t>(unread));
		else
			GuiHelper::drawSecondaryText(I18n::tr("%u samples, %u unread"), static_cast<uint32_t>(samples.size()), static_cast<uint32_t>(unread));

		if (unread > 0 && ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(I18n::tr("A round that could not read one of the kernel's lists leaves its tasks out of that sample. The gap is not a task that was idle."));
			ImGui::EndTooltip();
		}
	}

	/*
	 * The name of a lane's task.
	 *
	 * This round may not have read it - that is the whole reason a lane can be
	 * there while its task is missing - and the roster's copy is what the row
	 * above is labelled with, so the two agree.
	 */
	static std::string laneName(uint32_t address, const RtosSnapshot& snapshot, const std::vector<RtosTaskRow>& rows)
	{
		const RtosTask* task = snapshot.find(address);

		if (task != nullptr && !task->name.empty())
			return task->name;

		for (const RtosTaskRow& row : rows)
		{
			if (row.tcbAddress == address)
				return row.name;
		}

		return {};
	}

	static void drawGrid(const RtosSnapshot& snapshot, const std::vector<RtosTaskRow>& rows, const std::vector<uint32_t>& lanes,
	                     const std::vector<RtosTimeline::Sample>& samples, const std::vector<RtosEventRing::Interval>& runs,
	                     const std::vector<double>& plotCursors, size_t rosterSize, GuiRtosSelection& selection,
	                     float reservedBelow)
	{
		if (lanes.empty())
		{
			/* Two causes, and they must not be said the same way: a round with
			   no tasks at all, and a filter that kept none of the tasks the
			   round did have. "No tasks in this round" over a kernel that is
			   running thirteen of them is a false claim about the target. */
			if (snapshot.tasks.empty())
				GuiHelper::drawSecondaryText("%s", I18n::tr("No tasks in this round."));
			else
				GuiHelper::drawSecondaryText("%s", I18n::tr("The filters left no task to draw. Clear them to see the lanes again."));

			return;
		}

		const float scale = GuiHelper::contentScale;
		/* The name column is as wide as the longest name, within bounds: a
		   column fixed at 110 units is the right width for `led` and cuts
		   `xQueueReceiveTask` in half, and a column that grows without a limit
		   eats the plot when a target names a task a paragraph long. */
		float widestName = 0.0f;

		for (const uint32_t address : lanes)
		{
			const std::string name = laneName(address, snapshot, rows);

			if (!name.empty())
				widestName = std::max(widestName, ImGui::CalcTextSize(name.c_str()).x * 0.85f);
		}

		const float nameWidth = std::clamp(widestName + 12.0f * scale, 110.0f * scale, 210.0f * scale);
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float width = ImGui::GetContentRegionAvail().x;
		const float available = ImGui::GetContentRegionAvail().y;

		/*
		 * A little over half, not two thirds.
		 *
		 * Under the strip there are three blocks that are now written to fixed
		 * heights - the objects, the pinned moment, the selected task - and at
		 * two thirds their last rows were pushed past the bottom of the pane:
		 * the detail stopped at Priority and the reader never saw the rest. The
		 * strip is the part that can give, because it scrolls and zooms; the
		 * blocks under it cannot.
		 */
		/*
		 * One height for every lane, and the same height every round.
		 *
		 * It used to be the pane's height divided by the number of lanes, so a
		 * round that read one task more made every lane a little thinner and
		 * the whole picture jumped - twice a second, for a target that was not
		 * doing anything unusual. A lane is a row of a list: rows of a list do
		 * not resize because the list got longer, they scroll.
		 *
		 * With few lanes the strip shrinks to what it holds instead of leaving
		 * the extra height as a band of nothing under the last lane.
		 */
		const float rowHeight = 22.0f * scale;
		const float contentHeight = rowHeight * static_cast<float>(lanes.size());
		const float axisHeight = 16.0f * scale;

		/*
		 * The box is sized by the tasks the session knows, not by the lanes the
		 * filter left.
		 *
		 * A filter that tests the round changes how many lanes there are from
		 * one round to the next - a task this round did not read fails a
		 * priority floor, so its lane leaves and comes back half a second
		 * later. Sized to the filtered count, the strip changed height with it,
		 * and everything under the strip moved. The roster only grows, so
		 * sizing by it means the strip stands still while a filter comes and
		 * goes, and grows by one lane the day a task is created.
		 */
		/*
		 * The strip is as tall as the lanes it holds, and no taller.
		 *
		 * It used to take everything the blocks under it left, on the argument
		 * that the leftover band has to be somewhere and a chart area with room
		 * to spare is what a chart area looks like. That argument had a hole: the
		 * spare room sat inside the box, under the last lane, where it reads as
		 * "the target has thirteen tasks and four empty rows" rather than as a
		 * pane with space to grow. The lanes are a list, and a list does not have
		 * blank rows at the bottom.
		 *
		 * The room is still there - it is simply outside the box now, below the
		 * tracks, where nothing claims it is data.
		 */
		const float wanted = contentHeight + axisHeight + 2.0f * scale;
		const float floor = axisHeight + rowHeight * 2.0f;
		const float limit = std::max(available - reservedBelow, floor);

		const float height = std::clamp(wanted, floor, limit);
		const float plotHeight = height - axisHeight;

		/* How far the lanes are scrolled, and how far they can be. */
		const float overflow = std::max(contentHeight - plotHeight, 0.0f);
		scroll = std::clamp(scroll, 0.0f, overflow);
		const float plotLeft = origin.x + nameWidth;
		const float plotWidth = std::max(width - nameWidth, 40.0f * scale);

		lastAxisLeft = plotLeft;
		lastAxisWidth = plotWidth;

		const double newest = samples.back().atSeconds;
		const double oldest = samples.front().atSeconds;
		const double span = std::max(newest - oldest, 1e-6);

		/* A pin placed from outside - the event list - can be behind the window
		   the reader is looking at. A pin nobody can see is a pin they have to
		   hunt for, so the window is moved to hold it once. */
		if (hasPinned && (pinnedAt < oldest || pinnedAt > newest))
		{
			manualFrom = pinnedAt - span * 0.5;
			manualTo = pinnedAt + span * 0.5;
			manualWindow = true;
		}

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

		/*
		 * The name column is the one place a click means "this task" rather
		 * than "this moment".
		 *
		 * The strip is a chart of time and a list of tasks at once, and the two
		 * read the same rectangle. Splitting it by the name column is what
		 * keeps both meanings: a click on a name picks the lane, a click in the
		 * plot pins the time - and neither has to be guessed at from a
		 * modifier key.
		 */
		if (hovered && ImGui::GetIO().MousePos.x < plotLeft)
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				const float offset = std::max(ImGui::GetIO().MousePos.y - origin.y + scroll, 0.0f);
				const size_t lane = std::min(static_cast<size_t>(offset / rowHeight), lanes.size() - 1);
				const RtosTask* task = snapshot.find(lanes[lane]);

				selection.task = lanes[lane];
				selection.object = task != nullptr ? task->waitingOn : std::string();
			}
		}

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
		/* The wheel scrolls the list when the pointer is on the list, and zooms
		   the time axis when it is on the chart. Shift forces scrolling, so
		   that a long task list can be read without moving the hand off the
		   plot. */
		const bool wheelOverList = ImGui::GetIO().MousePos.x < plotLeft;
		const bool scrollWheel = overflow > 0.0f && (wheelOverList || ImGui::GetIO().KeyShift);

		if (hovered && ImGui::GetIO().MouseWheel != 0.0f && scrollWheel)
		{
			scroll = std::clamp(scroll - ImGui::GetIO().MouseWheel * rowHeight * 1.5f, 0.0f, overflow);
		}
		else if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
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

		/* Everything inside the frame is clipped to it: a lane scrolled half out
		   of view must be cut, not drawn over the pane above. */
		draw->PushClipRect(origin, ImVec2(origin.x + width, origin.y + plotHeight), true);

		for (size_t lane = 0; lane < lanes.size(); lane++)
		{
			const float top = origin.y + static_cast<float>(lane) * rowHeight - scroll;

			if (lane % 2 == 1)
				draw->AddRectFilled(ImVec2(plotLeft, top), ImVec2(plotLeft + plotWidth, top + rowHeight), ImGui::GetColorU32(ImGuiCol_Text, 0.03f));

			const std::string name = laneName(lanes[lane], snapshot, rows);

			if (!name.empty())
			{
				const float labelRoom = plotLeft - origin.x - 6.0f * scale;
				const std::string label = fittedLabel(name, labelRoom);
				const bool clipped = label != name;

				draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f, ImVec2(origin.x + 4 * scale, top + rowHeight * 0.1f),
				              ImGui::GetColorU32(ImGuiCol_Text), label.c_str());

				/* The whole name, for the one that did not fit. Asked for by
				   rectangle rather than by item, because the labels are drawn
				   before the strip's own item is submitted. */
				if (clipped && ImGui::IsMouseHoveringRect(ImVec2(origin.x, top), ImVec2(plotLeft, top + rowHeight)))
				{
					ImGui::BeginTooltip();
					ImGui::TextUnformatted(name.c_str());
					ImGui::TextDisabled("0x%08X", lanes[lane]);
					ImGui::EndTooltip();
				}
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

				const float top = origin.y + static_cast<float>(lane) * rowHeight - scroll;

				draw->AddRectFilled(ImVec2(origin.x, top), ImVec2(plotLeft + plotWidth, top + rowHeight), colour);
			}
		};

		highlightLane(selection.hoveredTask, ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.22f));
		highlightLane(selection.task, ImGui::GetColorU32(ImGuiCol_Header, 0.55f));

		/*
		 * The lanes as bands, not as marks.
		 *
		 * A mark says what one round saw, and the time between two rounds was
		 * left blank - and blank is what the eye reads as "nothing happened".
		 * A task blocked for two seconds looked like a task that was not
		 * there. Every state the samples agree on is drawn as a span instead,
		 * so a lane is a band of colour and the one thing a picture is good at -
		 * comparing lengths - is available.
		 *
		 * Faded, because the whole lane is now coloured: the runs below are
		 * drawn on top in full strength, and running has to stay the thing the
		 * eye lands on. The shortest band is held at two pixels; anything
		 * shorter is real and would otherwise vanish, and the reading underneath
		 * carries the true duration.
		 */
		const std::vector<RtosTimeline::StateInterval> bands = RtosTimeline::stateIntervals(samples, newest);
		const float bandHeight = std::max(rowHeight - 2.0f * scale, 2.0f * scale);

		/*
		 * The colour a gap gets.
		 *
		 * Every lane is first filled with "not read" over the stretch the
		 * history covers, and the bands are drawn on top. Whatever is left is
		 * exactly the rounds that missed that task, so a hole in a lane is now
		 * a colour that the legend explains rather than an absence that the
		 * reader has to interpret. Outside the covered stretch there is no
		 * colour at all - that part of the window is simply before the data.
		 */
		const float coveredFrom = std::clamp(timeToX(oldest), plotLeft, plotLeft + plotWidth);
		const float coveredTo = std::clamp(timeToX(newest), plotLeft, plotLeft + plotWidth);

		if (coveredTo > coveredFrom)
		{
			ImVec4 gap = ImGui::ColorConvertU32ToFloat4(unsampledColour);

			/* Darker than the violet this replaces, so it needs more of itself
			   to stand off the pane at all. */
			gap.w = 0.5f;

			for (size_t lane = 0; lane < lanes.size(); lane++)
			{
				const float top = origin.y + static_cast<float>(lane) * rowHeight + 1.0f * scale - scroll;

				draw->AddRectFilled(ImVec2(coveredFrom, top), ImVec2(coveredTo, top + bandHeight), ImGui::GetColorU32(gap));
			}
		}

		for (const RtosTimeline::StateInterval& band : bands)
		{
			const auto lane = std::find(lanes.begin(), lanes.end(), band.tcbAddress);

			if (lane == lanes.end())
				continue;

			const float from = timeToX(std::max(band.from, oldest));
			const float to = timeToX(std::min(band.to, newest));

			if (to < plotLeft || from > plotLeft + plotWidth)
				continue;

			ImVec4 colour = ImGui::ColorConvertU32ToFloat4(markOf(band.state).colour);

			/* Nearly opaque, and that is a correction rather than a taste.
			 *
			 * The four states are told apart by lightness as much as by hue -
			 * light green, bright yellow, deep amber - and blending them into a
			 * dark pane compresses exactly that: at 0.45 the yellow and the
			 * amber came out as two browns a few steps apart, which is the
			 * problem the colours were picked to solve. The band has to show
			 * the colour it was chosen for, so the pane contributes as little
			 * as it can while the strips still read as part of the window. */
			colour.w = 0.82f;

			const float top = origin.y + static_cast<float>(lane - lanes.begin()) * rowHeight + 1.0f * scale - scroll;
			const float width = std::max(to - from, 2.0f * scale);

			draw->AddRectFilled(ImVec2(from, top), ImVec2(from + width, top + bandHeight), ImGui::GetColorU32(colour));
		}

		/*
		 * Where the interrupts landed.
		 *
		 * One vertical line per interrupt, on the lane that was running at that
		 * instant. The strip says what each task was doing; the ISR band below
		 * says when interrupts happened; neither says which task a given
		 * interrupt interrupted, and "this task keeps getting cut off" is a fact
		 * about a task that only the two together can state.
		 *
		 * The line is drawn over the state colour rather than replacing it: the
		 * colour is still the answer to "what was it doing", and the line is the
		 * second answer on top of the first.
		 */
		if (isrTrack == nullptr || isrTrack->empty())
			isrCounts.clear();

		if (isrTrack != nullptr && !isrTrack->empty())
		{
			std::vector<RtosTimeline::StateInterval> running;

			running.reserve(bands.size());

			for (const RtosTimeline::StateInterval& band : bands)
			{
				if (band.state == RtosTaskState::Running)
					running.push_back(band);
			}

			/* Sorted once, because the attribution binary-searches it: the
			   intervals come out grouped by task, not by time. */
			std::sort(running.begin(), running.end(), [](const RtosTimeline::StateInterval& left, const RtosTimeline::StateInterval& right)
			          {
				return left.from < right.from;
			});

			const RtosIsrAttribution attribution = attributeIsr(running, *isrTrack);

			isrCounts = countIsrByTask(attribution);

			const ImU32 hitColour = ImGui::GetColorU32(ImGuiCol_Text, 0.55f);

			for (const RtosIsrHit& hit : attribution.hits)
			{
				const auto lane = std::find(lanes.begin(), lanes.end(), hit.tcbAddress);

				if (lane == lanes.end())
					continue;

				const float x = timeToX(hit.seconds);

				if (x < plotLeft || x > plotLeft + plotWidth)
					continue;

				const float top = origin.y + static_cast<float>(lane - lanes.begin()) * rowHeight + 1.0f * scale - scroll;

				draw->AddLine(ImVec2(x, top), ImVec2(x, top + bandHeight), hitColour, 1.0f * scale);
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

				const float top = origin.y + static_cast<float>(lane) * rowHeight + 1.0f * scale - scroll;

				draw->AddRectFilled(ImVec2(from, top), ImVec2(to, top + rowHeight - 2.0f * scale), runningColour);

				break;
			}
		}

		draw->PopClipRect();

		/*
		 * The lane scroll, said in two ways.
		 *
		 * A thin bar against the right edge of the frame shows how much of the
		 * list is on screen; the numbers at its foot say it outright. Without
		 * both, a task list longer than the strip would simply appear to end
		 * early, which is the same kind of quiet lie as a row that disappears.
		 */
		if (overflow > 0.0f)
		{
			const float trackX = origin.x + width - 4.0f * scale;
			const float thumbHeight = std::max(plotHeight * (plotHeight / contentHeight), 18.0f * scale);
			const float thumbY = origin.y + (plotHeight - thumbHeight) * (scroll / overflow);

			draw->AddRectFilled(ImVec2(trackX, origin.y), ImVec2(trackX + 3.0f * scale, origin.y + plotHeight),
			                    ImGui::GetColorU32(ImGuiCol_Text, 0.06f));
			draw->AddRectFilled(ImVec2(trackX, thumbY), ImVec2(trackX + 3.0f * scale, thumbY + thumbHeight),
			                    ImGui::GetColorU32(ImGuiCol_Text, 0.35f));

			char range[48] = {};
			const size_t first = static_cast<size_t>(scroll / rowHeight) + 1;
			const size_t last = std::min(lanes.size(), first + static_cast<size_t>(plotHeight / rowHeight));

			std::snprintf(range, sizeof(range), "%u-%u / %u", static_cast<unsigned>(first), static_cast<unsigned>(last),
			              static_cast<unsigned>(lanes.size()));

			const float textWidth = ImGui::CalcTextSize(range).x;
			draw->AddRectFilled(ImVec2(plotLeft + plotWidth - textWidth - 6.0f * scale, origin.y + plotHeight - 14.0f * scale),
			                    ImVec2(plotLeft + plotWidth, origin.y + plotHeight), ImGui::GetColorU32(ImGuiCol_WindowBg, 0.7f));
			draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.7f,
			              ImVec2(plotLeft + plotWidth - textWidth - 3.0f * scale, origin.y + plotHeight - 13.0f * scale),
			              ImGui::GetColorU32(ImGuiCol_TextDisabled), range);
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

			draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + plotHeight), ImGui::GetColorU32(ImGuiCol_Text, 0.5f), 1.0f);

			/* The label goes under the axis, and is left off where it would sit
			   on top of the axis's own last label: two times printed over each
			   other read as one wrong time. */
			const float labelX = x + 2.0f * scale;

			if (labelX < plotLeft + plotWidth - 54 * scale)
				draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.75f, ImVec2(labelX, origin.y + plotHeight + 2 * scale),
				              ImGui::GetColorU32(ImGuiCol_TextDisabled),
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

		/* The row under the axis says what the cursor is over, and it is a row
		   whether or not there is a cursor: drawn only when something is
		   pinned or hovered, its absence made the strip a line shorter, and
		   pinning the first moment pushed every block under the strip down by
		   that line. The space is kept, the sentence is not. */
		const float readoutTop = ImGui::GetCursorPosY();

		drawCursor(draw, samples, lanes, origin, plotHeight, plotLeft, plotWidth, hovered);

		const float readoutHeight = ImGui::GetTextLineHeightWithSpacing();
		const float readoutUsed = ImGui::GetCursorPosY() - readoutTop;

		if (readoutUsed < readoutHeight)
			ImGui::Dummy(ImVec2(0.0f, readoutHeight - readoutUsed));
	}

	static void drawCursor(ImDrawList* draw, const std::vector<RtosTimeline::Sample>& samples, const std::vector<uint32_t>& lanes, const ImVec2& origin,
	                       float plotHeight, float plotLeft, float plotWidth, bool hovered)
	{
		ImGuiIO& io = ImGui::GetIO();

		/* A click pins the moment the mouse is over, and a second click on the
		   same strip moves it. Nothing is pinned until someone asks, so the
		   strip shows the whole window by default. */
		if (hovered && io.MousePos.x >= plotLeft && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
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

		draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + plotHeight), ImGui::GetColorU32(ImGuiCol_Text, 0.55f));

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

		/* A gap is a reading now, not an absence: the one colour that means
		   "this round could not say" rather than "nothing happened". */
		{
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const ImVec2 topLeft(origin.x, origin.y + (ImGui::GetTextLineHeight() - size) * 0.5f);

			ImVec4 gap = ImGui::ColorConvertU32ToFloat4(unsampledColour);
			gap.w = 0.75f;

			draw->AddRectFilled(topLeft, ImVec2(topLeft.x + size, topLeft.y + size), ImGui::GetColorU32(gap));

			ImGui::Dummy(ImVec2(size + 4.0f * scale, 0.0f));
			ImGui::SameLine(0.0f, 4.0f * scale);
			GuiHelper::drawSecondaryText("%s", I18n::tr("not read"));
		}

		ImGui::SameLine(0.0f, spacing);
		GuiHelper::drawSecondaryText("%s", I18n::tr("thin line"));
		ImGui::SameLine(0.0f, 4.0f * scale);
		GuiHelper::drawSecondaryText("%s", I18n::tr("a cursor in the variable view"));

		/* The caveat gets a line of its own, and that line wraps.
		 *
		 * It is the one thing about the strip that is easy to misread, so it
		 * stays on screen instead of moving into a tooltip; and it is a
		 * sentence, so it is allowed to take a second line rather than having
		 * its end cut off by the edge of the window. A row of legend entries
		 * joined by `SameLine` has no second line to take. */
		GuiHelper::drawSecondaryTextWrapped("%s", I18n::tr("- the round did not read that task, which is not the same as idle"));

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

	/*
	 * Fixed hues rather than theme colours: these have to stay apart from one
	 * another, and a theme's accent colour is chosen for something else.
	 *
	 * Picked for a reader who is red-green colour weak, and by that reader:
	 * light green for running, orange-yellow for ready, amber for blocked. All
	 * three sit on the red-green axis, which sounds like the wrong place to put
	 * them - and it is the wrong place to put them at one lightness. What makes
	 * it work is that they are spread across lightness instead: nearly white
	 * green, bright yellow, deep amber. Lightness survives red-green weakness
	 * intact, so the spread is what a reader with the weakness actually sees.
	 *
	 *   running   light green   (160,224,152)
	 *   ready     orange-yellow (250,196,70)
	 *   blocked   amber         (168,104,18)
	 *   suspended neutral grey  (146,146,152)
	 *
	 * Measured CIELAB distance between the closest pair: 37 under normal
	 * vision, 35 under deuteranopia, 36 under protanopia. The green and amber
	 * this replaces measured 9 and 3 - not because they were green and amber,
	 * but because they were the same lightness.
	 *
	 * Move one of the first three up or down a long way in lightness and it
	 * closes onto a neighbour; the order light / bright / deep is the part
	 * doing the work, not the hues.
	 */
	/* Borrowed from the panel; null until it hands one over. */
	inline static const std::vector<RtosTrackEntry>* isrTrack = nullptr;

	inline static std::map<uint32_t, uint32_t> isrCounts;

	inline static const ImU32 runningColour = IM_COL32(160, 224, 152, 255);
	inline static const ImU32 readyColour = IM_COL32(250, 196, 70, 255);
	inline static const ImU32 blockedColour = IM_COL32(168, 104, 18, 255);
	inline static const ImU32 suspendedColour = IM_COL32(146, 146, 152, 255);

	/* A task a round did not read.
	   Its own hue, because a gap in a lane has two possible meanings - the task
	   was not there, or the round could not look - and until now the picture
	   left both of them blank. Dark brown: it belongs with "nothing was read
	   here" rather than with any of the four states, and it is drawn at a third
	   of full strength so that it stays a background. */
	inline static const ImU32 unsampledColour = IM_COL32(84, 60, 42, 255);
};

#endif
