#include <implot.h>

#include "Gui.hpp"
#include "I18n.hpp"

void Gui::drawPlotsSwo()
{
	ImVec2 plotSize(-1, -1);

	float rowRatios[] = {1.2f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

	/* A trace view with every channel hidden has no rows to draw, and ImPlot
	   refuses a grid of none. One row is the smallest thing that can be handed
	   over; the loop below then draws nothing into it. */
	const int visiblePlots = tracePlotHandler->getVisiblePlotsCount();
	const int rows = visiblePlots > 0 ? visiblePlots : 1;

	if (ImPlot::BeginSubplots("##subplos", rows, 1, plotSize, ImPlotSubplotFlags_LinkAllX, rowRatios))
	{
		bool first = true;
		for (std::shared_ptr<Plot> plt : *tracePlotHandler)
		{
			if (!plt->getVisibility())
				continue;

			drawPlotCurveSwo(plt.get(), *plt->getXAxisSeries(), plt->getSeriesMap(), first);
			first = false;
		}
		ImPlot::EndSubplots();
	}
}

void Gui::drawPlotCurveSwo(Plot* plot, ScrollingBuffer<double>& time, std::map<std::string, std::shared_ptr<Plot::Series>>& seriesMap, bool first)
{
	if (ImPlot::BeginPlot(plot->getName().c_str(), ImVec2(), ImPlotFlags_NoChild | ImPlotFlags_NoTitle))
	{
		recordDrawnPlot(plot->getName());

		if (first)
			ImPlot::SetupAxis(ImAxis_X1, I18n::tr("time[s]"), ImPlotAxisFlags_Opposite | ImPlotAxisFlags_NoLabel);
		else
			ImPlot::SetupAxis(ImAxis_X1, I18n::tr("time[s]"), ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoLabel);

		const bool running = traceDataHandler->getStateImmediate() == DataHandlerBase::State::RUN;
		const bool follows = plot->isViewFollowingLatestData();

		/* Everything ImPlot is asked about this plot goes here, before any call
		   that is not part of the setup. ImPlot locks the setup for the rest of
		   the frame as soon as such a call is made - reading the hover state is
		   one of them - and SetupAxis asserts once it is locked. */
		if (plot->getDomain() == Plot::Domain::DIGITAL)
			ImPlot::SetupAxis(ImAxis_Y1, NULL, ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoLabel);
		else
			ImPlot::SetupAxis(ImAxis_Y1, NULL, ImPlotAxisFlags_None);

		if (running)
		{
			if (follows)
			{
				auto settings = traceDataHandler->getSettings();
				const double min = time.getOldestValue();
				const double max = time.getNewestValue();
				const double viewportWidth = (max - min) * (settings.maxViewportPointsPercent / 100.0);
				ImPlot::SetupAxisLimits(ImAxis_X1, max - viewportWidth, max, ImPlotCond_Always);
			}
		}

		if (plot->getDomain() == Plot::Domain::DIGITAL)
			ImPlot::SetupAxisLimits(ImAxis_Y1, -0.25, 1.25, ImPlotCond_Always);

		/* This subplot links the horizontal axis of every trace plot to one
		   shared pair of numbers, so the view leaves the live end for all of
		   them at once: whichever plot stopped writing the range would be
		   pulled back by the next one still writing it.

		   It sits after the setup on purpose: it asks whether the plot is
		   hovered, and that call locks the setup. */
		if (running && follows && plotViewWasAskedForByHand())
		{
			for (const std::shared_ptr<Plot>& other : *tracePlotHandler)
				other->setViewFollowsLatestData(false);
		}

		Plot::Series* ser = plot->getSeriesMap().begin()->second.get();

		if (plot->trigger.getState())
		{
			auto triggerLevel = plot->trigger.getValue();
			ImPlot::DragLineY(0, &triggerLevel, GuiHelper::triggerLine(), 1.0f);
			plot->trigger.setValue(triggerLevel);
		}

		if (traceDataHandler->getStateImmediate() == DataHandlerBase::State::STOP)
		{
			ImPlotRect plotLimits = ImPlot::GetPlotLimits();

			if (plot->drawsXCursors())
				CursorRenderer::drawX(plot, plotLimits);

			if (plot->drawsYCursors())
				CursorRenderer::drawY(plot, plotLimits);

			handleDragRect(0, plot->stats, plotLimits);
		}

		plot->setIsHovered(ImPlot::IsPlotHovered());

		/* make thread safe copies of buffers - TODO refactor */
		mtx->lock();
		time.copyData();
		if (ser->visible)
			ser->buffer->copyData();
		uint32_t offset = time.getOffset();
		uint32_t size = time.getSize();
		mtx->unlock();

		ImPlot::SetNextLineStyle(GuiHelper::traceColor(ser->var->getColor().r, ser->var->getColor().g, ser->var->getColor().b));
		ImPlot::SetNextFillStyle(GuiHelper::traceColor(ser->var->getColor().r, ser->var->getColor().g, ser->var->getColor().b), 0.25f);
		ImPlot::PlotStairs(plot->getAlias().c_str(), time.getFirstElementCopy(), ser->buffer->getFirstElementCopy(), size, ImPlotStairsFlags_Shaded, offset, sizeof(double));

		/* The dot marks where the first X cursor cut the trace, so it only
		   exists while that cursor does. */
		if (plot->drawsXCursors())
		{
			const double timepoint = plot->markerX0.getValue();
			const double value = *(ser->buffer->getFirstElementCopy() + time.getIndexFromvalue(timepoint));

			ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, ImVec4(1, 1, 1, 1), 0.5f);
			ImPlot::PlotScatter("###point", &timepoint, &value, 1, false);
		}

		if (traceDataHandler->getStateImmediate() == DataHandlerBase::State::STOP)
		{
			/* Red for the frames that were not understood, yellow for the two
			   kinds that arrived late - which is how the documentation reads
			   them, and the data around either is not to be trusted.

			   The line of zeroes they hang on exists only to give the markers a
			   place on the axis, so it is built to the length of what is being
			   drawn rather than to a number written down elsewhere and left to
			   agree with it. */
			auto drawMarkers = [](const char* id, const std::vector<double>& timestamps, float markerSize, const ImVec4& colour)
			{
				if (timestamps.empty())
					return;

				const std::vector<double> zeroes(timestamps.size(), 0.0);

				ImPlot::SetNextMarkerStyle(ImPlotMarker_Cross, markerSize, colour, 3, colour);
				ImPlot::PlotScatter(id, timestamps.data(), zeroes.data(), timestamps.size(), false);
			};

			drawMarkers("###errorFrames", traceDataHandler->getErrorTimestamps(), 8, ImVec4(1, 0, 0, 1));
			drawMarkers("###delayed2Frames", traceDataHandler->getDelayed2Timestamps(), 4, ImVec4(1, 1, 0, 1));
			drawMarkers("###delayed3Frames", traceDataHandler->getDelayed3Timestamps(), 6, ImVec4(1, 1, 0, 1));
		}

		/* One control for the whole stack. The plots share a horizontal axis,
		   so they leave the live end together and come back to it together,
		   and one button on the top plot says so without repeating itself
		   down the stack. */
		if (running && first && drawBackToLiveButton(*plot))
		{
			for (const std::shared_ptr<Plot>& other : *tracePlotHandler)
				other->setViewFollowsLatestData(true);
		}

		ImPlot::EndPlot();
	}
}