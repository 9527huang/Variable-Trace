#include "Gui.hpp"
#include "I18n.hpp"

#include <cstdlib>
#include <string>
#include <unordered_map>

void Gui::dragAndDropPlot(std::shared_ptr<Plot> plot)
{
	if (ImPlot::BeginDragDropTargetPlot())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MY_DND"))
		{
			std::set<std::string>* selection = *(std::set<std::string>**)payload->Data;

			addSelectionToPlot(plot, *selection);
			selection->clear();
		}
		ImPlot::EndDragDropTarget();
	}
}

/* Puts the dragged variables on the plot, and asks for the set of addresses
   read from the target to be rebuilt so that they are actually sampled.

   Without the second half a variable dropped onto a running plot would be
   drawn only once the reader stopped and started again, because the addresses
   come from the plots and are rebuilt on a state change. The acquisition
   thread performs the rebuild where it already does that, which is what keeps
   it free of races; while stopped the request is ignored and the next start
   builds the list anyway. */
void Gui::addSelectionToPlot(std::shared_ptr<Plot> plot, const std::set<std::string>& selection)
{
	if (selection.empty())
		return;

	{
		/* The series names a variable through a bare pointer, and the
		   acquisition thread walks the same plots. */
		auto modelLock = variableHandler->lockModel();

		for (const std::string& name : selection)
		{
			if (variableHandler->contains(name))
				plot->addSeries(variableHandler->getVariable(name).get());
		}
	}

	viewerDataHandler->requestRestart();
}

/* Whether the input arriving this frame asks the plot being drawn for a view
   other than the one it is showing.

   Comparing the axis range before and after the frame is the obvious way to
   tell, and it does not work here. Reading a range back from ImPlot locks
   the rest of the setup for that plot, and a range written by this code in
   the same frame is the range that would be read back, so a change made by
   the reader between the two is never visible in either direction.

   The input is asked instead. A wheel, a drag with the pan button and a drag
   with the select button are the three ways a plot view can be moved, and
   the keys they sit on come from ImPlot's own map, so a gesture counts here
   exactly when ImPlot would have acted on it.

   Only the area the trace is drawn in is watched, so a wheel turned over an
   axis label is missed. It would move the same view from the same call, and
   a reader who wants to move a view puts the pointer on the trace, where
   they are looking. */
bool Gui::plotViewWasAskedForByHand()
{
	if (!ImPlot::IsPlotHovered())
		return false;

	/* A variable dragged onto a plot is held by the same button that pans
	   one, and that gesture belongs to the drop rather than to the view. */
	if (ImGui::GetDragDropPayload() != nullptr)
		return false;

	const ImGuiIO& io = ImGui::GetIO();
	const ImPlotInputMap& inputMap = ImPlot::GetInputMap();

	/* ImPlot reads a modifier as a requirement when the map names one and as
	   an absence of any requirement when it names none, which is what this
	   leaves it as. */
	auto modifierHeld = [&io](int modifier) { return (io.KeyMods & modifier) == modifier; };

	/* Ctrl is ImPlot's key for standing aside and letting whatever lies
	   under the plot have the mouse, so a gesture holding it is not meant
	   for the axes. */
	if (inputMap.OverrideMod != ImGuiMod_None && modifierHeld(inputMap.OverrideMod))
		return false;

	if (io.MouseWheel != 0.0f && modifierHeld(inputMap.ZoomMod))
		return true;

	if (ImGui::IsMouseDragging(inputMap.Pan) && modifierHeld(inputMap.PanMod))
		return true;

	return ImGui::IsMouseDragging(inputMap.Select) && modifierHeld(inputMap.SelectMod);
}

bool Gui::drawBackToLiveButton(const Plot& plot)
{
	if (plot.isViewFollowingLatestData())
		return false;

	const ImVec2 tracePos = ImPlot::GetPlotPos();
	const ImVec2 traceSize = ImPlot::GetPlotSize();

	const char* label = I18n::tr("Back to live");
	const ImVec2 labelSize = ImGui::CalcTextSize(label);
	const ImVec2 padding(8.0f * GuiHelper::contentScale, 4.0f * GuiHelper::contentScale);
	const ImVec2 buttonSize(labelSize.x + 2 * padding.x, labelSize.y + 2 * padding.y);

	/* Put it in the top right corner of the trace area. A view the reader
	   moved by hand is usually looking at older data, so the newest samples
	   are off to the right and the corner it is drawn in is the one they are
	   least likely to be reading. */
	const ImVec2 buttonPos(tracePos.x + traceSize.x - buttonSize.x - padding.x, tracePos.y + padding.y);
	const ImVec2 buttonEnd(buttonPos.x + buttonSize.x, buttonPos.y + buttonSize.y);
	const ImVec2 textPos(buttonPos.x + padding.x, buttonPos.y + padding.y);

	/* The control is read and drawn by hand rather than through
	   ImGui::Button. A plot answers the mouse over the whole of its area, and
	   a widget placed inside that area loses the press to it: ImGui would
	   have to be told which of the two comes first, and it cannot be told
	   without also changing how the plot itself takes input. Reading the
	   pointer here leaves the plot's own handling exactly as it was, and a
	   left click means nothing to a plot on its own - it pans only once it is
	   dragged. */
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const bool over = mouse.x >= buttonPos.x && mouse.x <= buttonEnd.x &&
					  mouse.y >= buttonPos.y && mouse.y <= buttonEnd.y;
	const bool pressed = over && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const ImU32 fill = ImGui::GetColorU32(over ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
	drawList->AddRectFilled(buttonPos, buttonEnd, fill, ImGui::GetStyle().FrameRounding);
	drawList->AddRect(buttonPos, buttonEnd, ImGui::GetColorU32(ImGuiCol_Border), ImGui::GetStyle().FrameRounding);
	drawList->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), label);

	return pressed;
}

void Gui::drawPlots()
{
	uint32_t tablePlots = 0;
	ImVec2 initialCursorPos = ImGui::GetCursorPos();
	auto activeGroup = plotGroupHandler->getActiveGroup();

	for (auto [name, plotElem] : *activeGroup)
	{
		auto plot = plotElem.plot;

		if (plot->getType() == Plot::Type::TABLE)
		{
			if (plotElem.visibility)
			{
				tablePlots++;
				drawPlotTable(plot);
			}
		}
	}

	uint32_t curveBarPlotsCnt = activeGroup->getVisiblePlotsCount() - tablePlots;
	uint32_t row = curveBarPlotsCnt > 0 ? curveBarPlotsCnt : 1;

	const float remainingSpace = (ImGui::GetWindowPos().y + ImGui::GetWindowSize().y) - (ImGui::GetCursorPos().y + initialCursorPos.y);
	ImVec2 plotSize(-1, -1);
	if (remainingSpace < 300)
		plotSize.y = 300;

	if (ImPlot::BeginSubplots("##subplos", row, 1, plotSize, 0))
	{
		auto activeGroup = plotGroupHandler->getActiveGroup();

		for (auto [name, plotElem] : *activeGroup)
		{
			auto plot = plotElem.plot;

			if (!plotElem.visibility)
				continue;

			if (plot->getType() == Plot::Type::CURVE)
				drawPlotCurve(plot);
			else if (plot->getType() == Plot::Type::BAR)
				drawPlotBar(plot);
			else if (plot->getType() == Plot::Type::XY)
				drawPlotXY(plot);
		}

		ImPlot::EndSubplots();
	}
}

void Gui::drawPlotXY(std::shared_ptr<Plot> plot)
{
	auto& time = *plot->getXAxisSeries();
	auto& seriesMap = plot->getSeriesMap();

	if (ImPlot::BeginPlot(plot->getName().c_str(), ImVec2(-1, -1), ImPlotFlags_NoChild))
	{
		recordDrawnPlot(plot->getName());

		const std::string xLabel = plot->getEffectiveXAxisLabel();
		const std::string yLabel = plot->getEffectiveYAxisLabel();

		const bool running = viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN;
		const bool follows = plot->isViewFollowingLatestData();

		/* Both axes are fitted while the view is being carried along. Once
		   the reader has taken the view over, fitting either of them again
		   would keep pulling back the axis they just moved. */
		if (running)
		{
			const ImPlotAxisFlags fit = follows ? ImPlotAxisFlags_AutoFit : ImPlotAxisFlags_None;
			ImPlot::SetupAxes(xLabel.c_str(), yLabel.c_str(), fit, fit);
		}
		else
		{
			ImPlot::SetupAxes(xLabel.c_str(), yLabel.c_str(), 0, 0);
			ImPlot::SetupAxisLimits(ImAxis_X1, -1, 10, ImPlotCond_Once);
			ImPlot::SetupAxisLimits(ImAxis_Y1, -0.1, 0.1, ImPlotCond_Once);
		}

		/* Asked only while the view is still being carried along, since
		   there is nothing left to hand over once the reader has it. */
		if (running && follows && plotViewWasAskedForByHand())
			plot->setViewFollowsLatestData(false);

		plot->setIsHovered(ImPlot::IsPlotHovered());
		dragAndDropPlot(plot);

		/* The cursors go on the same way here as on a time plot: two lines
		   each way, and the readings the rectangle they close produces. The
		   slope of that rectangle is the reading this kind of plot is
		   usually opened for, and it comes out of a pair on both
		   directions, so the mode is asked rather than assumed. */
		if (viewerDataHandler->getStateImmediate() == DataHandlerBase::State::STOP)
		{
			ImPlotRect plotLimits = ImPlot::GetPlotLimits();

			if (plot->drawsXCursors())
				CursorRenderer::drawX(plot.get(), plotLimits);

			if (plot->drawsYCursors())
				CursorRenderer::drawY(plot.get(), plotLimits, plot->drawsXCursors() ? plot->markerX1.getValue() - plot->markerX0.getValue() : 0.0);
		}

		/* make thread safe copies of buffers - TODO refactor */
		mtx->lock();
		time.copyData();
		for (auto& [key, serPtr] : seriesMap)
		{
			if (!serPtr->visible)
				continue;
			serPtr->buffer->copyData();
		}
		uint32_t offset = time.getOffset();
		uint32_t size = time.getSize();
		mtx->unlock();

		for (auto& [key, serPtr] : seriesMap)
		{
			if (!serPtr->visible)
				continue;

			ImPlot::SetNextLineStyle(GuiHelper::traceColor(serPtr->var->getColor().r, serPtr->var->getColor().g, serPtr->var->getColor().b));
			ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2.0f);
			ImPlot::PlotLine(key.c_str(), time.getFirstElementCopy(), serPtr->buffer->getFirstElementCopy(), size, 0, offset, sizeof(double));
		}

		if (running && drawBackToLiveButton(*plot))
			plot->setViewFollowsLatestData(true);

		ImPlot::EndPlot();
	}
}

void Gui::drawPlotCurve(std::shared_ptr<Plot> plot)
{
	auto& time = *plot->getXAxisSeries();
	auto& seriesMap = plot->getSeriesMap();

	if (ImPlot::BeginPlot(plot->getName().c_str(), ImVec2(-1, -1), ImPlotFlags_NoChild))
	{
		recordDrawnPlot(plot->getName());

		const std::string xLabel = plot->getEffectiveXAxisLabel();
		const std::string yLabel = plot->getEffectiveYAxisLabel();

		const bool running = viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN;
		const bool follows = plot->isViewFollowingLatestData();

		if (running)
		{
			ViewerDataHandler::Settings settings = viewerDataHandler->getSettings();

			/* The vertical axis is fitted to the data only while the view is
			   being carried along. Fitting it again after the reader has
			   taken the view over would keep pulling back the axis they just
			   moved. */
			ImPlot::SetupAxis(ImAxis_Y1, yLabel.c_str(), follows ? ImPlotAxisFlags_AutoFit : ImPlotAxisFlags_None);
			ImPlot::SetupAxis(ImAxis_X1, xLabel.c_str(), 0);

			if (follows)
			{
				/* The window is a length of time. How much of it can be filled
				   is decided by the samples that are still in hand, which is
				   what liveTimeWindow is about. */
				const double viewportWidth = settings.viewportWidthMs / 1000.0;
				const double* newest = time.getLastElement();

				/* A ring nothing has been put into yet has no newest sample to
				   hang the window on - which is what a run looks like for the
				   frames between the buffers being emptied and the first sample
				   arriving. The axis is left where it is until one does. */
				if (newest != nullptr)
				{
					const std::pair<double, double> window = Plot::liveTimeWindow(*newest, time.getOldestValue(), viewportWidth);
					ImPlot::SetupAxisLimits(ImAxis_X1, window.first, window.second, ImPlotCond_Always);
				}
			}
		}
		else
		{
			ImPlot::SetupAxes(xLabel.c_str(), yLabel.c_str(), 0, 0);
			ImPlot::SetupAxisLimits(ImAxis_X1, -1, 10, ImPlotCond_Once);
			ImPlot::SetupAxisLimits(ImAxis_Y1, -0.1, 0.1, ImPlotCond_Once);
		}

		/* Asked only while the view is still being carried along, since
		   there is nothing left to hand over once the reader has it. */
		if (running && follows && plotViewWasAskedForByHand())
			plot->setViewFollowsLatestData(false);

		plot->setIsHovered(ImPlot::IsPlotHovered());
		dragAndDropPlot(plot);

		if (viewerDataHandler->getStateImmediate() == DataHandlerBase::State::STOP)
		{
			ImPlotRect plotLimits = ImPlot::GetPlotLimits();

			if (plot->drawsXCursors())
				CursorRenderer::drawX(plot.get(), plotLimits);

			/* The slope a pair of vertical cursors stands for is only a
			   number when the same plot also has a pair along the bottom,
			   so the width of that pair is handed over with them. */
			if (plot->drawsYCursors())
				CursorRenderer::drawY(plot.get(), plotLimits, plot->drawsXCursors() ? plot->markerX1.getValue() - plot->markerX0.getValue() : 0.0);

			handleDragRect(0, plot->stats, plotLimits);
		}

		/* make thread safe copies of buffers - TODO refactor */
		mtx->lock();
		time.copyData();
		for (auto& [key, serPtr] : seriesMap)
		{
			if (!serPtr->visible)
				continue;
			serPtr->buffer->copyData();
		}
		uint32_t offset = time.getOffset();
		uint32_t size = time.getSize();
		mtx->unlock();

		for (auto& [key, serPtr] : seriesMap)
		{
			if (!serPtr->visible)
				continue;

			/* The value a series shows at the cursor is the one the
			   reading under its name reports, so it is asked for at the
			   first X cursor. Without that cursor there is no point on
			   the series to name, and the legend stays as it was. */
			std::string name = key;

			if (plot->drawsXCursors())
			{
				const double timepoint = plot->markerX0.getValue();
				const double value = *(serPtr->buffer->getFirstElementCopy() + time.getIndexFromvalue(timepoint));
				name = key + " = " + Plot::formatCursorValue(value);
			}

			ImPlot::SetNextLineStyle(GuiHelper::traceColor(serPtr->var->getColor().r, serPtr->var->getColor().g, serPtr->var->getColor().b));
			ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2.0f);
			ImPlot::PlotLine(name.c_str(), time.getFirstElementCopy(), serPtr->buffer->getFirstElementCopy(), size, ImPlotLineFlags_None, offset, sizeof(double));
		}

		if (running && drawBackToLiveButton(*plot))
			plot->setViewFollowsLatestData(true);

		ImPlot::EndPlot();
	}
}
void Gui::drawPlotBar(std::shared_ptr<Plot> plot)
{
	auto& seriesMap = plot->getSeriesMap();

	if (ImPlot::BeginPlot(plot->getName().c_str(), ImVec2(-1, -1), ImPlotFlags_NoChild))
	{
		recordDrawnPlot(plot->getName());

		std::vector<const char*> glabels;
		std::vector<double> positions;

		float pos = 0.0f;
		float visiblePlotsCnt = 0;
		for (const auto& [name, series] : seriesMap)
		{
			if (!series->visible)
				continue;

			glabels.push_back(name.c_str());
			positions.push_back(pos);
			pos += 1.0f;
			visiblePlotsCnt++;
		}
		glabels.push_back(nullptr);

		ImPlot::SetupAxes(plot->getEffectiveXAxisLabel().c_str(), plot->getEffectiveYAxisLabel().c_str(), 0, 0);
		ImPlot::SetupAxisLimits(ImAxis_X1, -1, visiblePlotsCnt, ImPlotCond_Always);
		ImPlot::SetupAxisTicks(ImAxis_X1, positions.data(), visiblePlotsCnt, glabels.data());

		dragAndDropPlot(plot);

		double xs = 0.0f;
		double barSize = 0.5f;

		for (auto& [name, series] : seriesMap)
		{
			if (!series->visible)
				continue;
			double value = *series->buffer->getLastElement();

			ImPlot::SetNextLineStyle(GuiHelper::traceColor(series->var->getColor().r, series->var->getColor().g, series->var->getColor().b));
			ImPlot::SetNextFillStyle(GuiHelper::traceColor(series->var->getColor().r, series->var->getColor().g, series->var->getColor().b));
			ImPlot::PlotBars(name.c_str(), &xs, &value, 1, barSize);
			ImPlot::Annotation(xs, value / 2.0f, ImVec4(0, 0, 0, 0), ImVec2(0, -5 * GuiHelper::contentScale), true, "%.5f", value);
			xs += 1.0f;
		}
		ImPlot::EndPlot();
	}
}

void Gui::drawPlotTable(std::shared_ptr<Plot> plot)
{
	auto& seriesMap = plot->getSeriesMap();

	static ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable;

	ImGui::SetCursorPosX((ImGui::GetWindowSize().x - ImGui::CalcTextSize(plot->getName().c_str()).x) * 0.5f);
	ImGui::Text("%s", plot->getName().c_str());

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));  // Adjust vertical padding
	if (ImGui::BeginTable(plot->getName().c_str(), 4, flags))
	{
		ImGui::TableSetupScrollFreeze(0, 1);  // Make top row always visible

		/* Translated header, English id: the widths the reader drags are filed
		   under the id, so they outlive a change of language. */
		ImGui::TableSetupColumn(I18n::tr("Name"), ImGuiTableColumnFlags_None, 0.0f, ImGui::GetID("Name"));
		ImGui::TableSetupColumn(I18n::tr("Address"), ImGuiTableColumnFlags_None, 0.0f, ImGui::GetID("Address"));
		ImGui::TableSetupColumn(I18n::tr("Read value"), ImGuiTableColumnFlags_None, 0.0f, ImGui::GetID("Read value"));
		ImGui::TableSetupColumn(I18n::tr("Write value"), ImGuiTableColumnFlags_None, 0.0f, ImGui::GetID("Write value"));
		ImGui::TableHeadersRow();

		for (auto& [key, serPtr] : seriesMap)
		{
			if (!serPtr->visible)
				continue;

			ImGui::BeginDisabled(!serPtr->var->getIsCurrentlySampled() && viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN);

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			Variable::Color color = serPtr->var->getColor();
			ImVec4 col = GuiHelper::traceColor(color.r, color.g, color.b, color.a);
			ImGui::ColorButton("##", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoTooltip, ImVec2(10 * GuiHelper::contentScale, 10 * GuiHelper::contentScale));
			ImGui::SameLine();
			ImGui::Text("%s", key.c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%s", ("0x" + std::string(GuiHelper::intToHexString(serPtr->var->getAddress()))).c_str());
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%s", plot->getSeriesValueString(key, serPtr->var->getValue()).data());
			showChangeFormatPopup("format", *plot, key);
			ImGui::TableSetColumnIndex(3);

			/* A new value only goes out while the acquisition is running: the
			   sampling thread is reading over the same link, and the write is
			   answered on it. So the cell is disabled rather than silently
			   swallowing what was typed. */
			const bool writeAvailable = (viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN);

			/* What was typed has to outlive the frame that drew it. The button
			   reports its press on the frame the mouse is released, which is
			   after the field has already been deactivated - and a buffer that
			   was thrown away at the end of the previous frame holds nothing by
			   then, so the press writes an empty value. Kept per row, so two
			   rows do not share what was typed into them. */
			static std::unordered_map<std::string, std::string> writeText;
			std::string& valueToWrite = writeText[plot->getName() + "##" + key];

			std::string inputName = "##Input" + key;

			/* Every row carries a button with the same words on it, so the id
			   has to come from somewhere else or the rows share one. */
			std::string writeLabel = std::string(I18n::tr("Write")) + "##Write" + key;

			ImVec4 cellBgColor = ImGui::GetStyle().Colors[ImGuiCol_TableRowBg];
			ImGui::PushStyleColor(ImGuiCol_FrameBg, cellBgColor);
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive, cellBgColor);
			ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, cellBgColor);

			ImGui::BeginDisabled(!writeAvailable);

			const float writeButtonWidth = ImGui::CalcTextSize(I18n::tr("Write")).x + 2.0f * ImGui::GetStyle().FramePadding.x;
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - writeButtonWidth - ImGui::GetStyle().ItemSpacing.x);

			/* Enter is taken by the field itself rather than looked for
			   afterwards: a plain InputText reads Enter as "done with this
			   field" and reports no change, so a write keyed off the change
			   never fires. EnterReturnsTrue makes the press come back as the
			   field's own return value. */
			const bool entered = ImGui::InputText(inputName.c_str(), &valueToWrite,
												 ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);

			ImGui::SameLine();
			const bool writePressed = ImGui::Button(writeLabel.c_str(), ImVec2(writeButtonWidth, 0));

			if (entered || writePressed)
			{
				/* CharsDecimal still lets a lone "-" or "." through, and
				   std::stod throws on those - on this thread that ends the
				   program, so the number is read with an answer for "not a
				   number" instead. */
				const char* text = valueToWrite.c_str();
				char* end = nullptr;
				const double value = std::strtod(text, &end);
				const bool isNumber = (end != text) && (*end == '\0');

				if (!isNumber)
					logger->error("'{}' is not a number, nothing was written", valueToWrite);
				else
				{
					logger->info("New value to be written: {}", valueToWrite);

					if (!viewerDataHandler->writeVariable(*serPtr->var, value))
					{
						const std::string reason = viewerDataHandler->getLastWriteError();
						logger->error("Error while writing new value! {}", reason);
					}
				}
			}

			ImGui::EndDisabled();
			ImGui::PopStyleColor(3);
			ImGui::EndDisabled();
		}
		ImGui::EndTable();

		/* A table has no plot area to ask for, so its rectangle is taken from
		   the table item that was just closed. */
		recordDrawnPlot(plot->getName(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MY_DND"))
			{
				std::set<std::string>* selection = *(std::set<std::string>**)payload->Data;

				addSelectionToPlot(plot, *selection);
				selection->clear();
			}
			ImGui::EndDragDropTarget();
		}
	}
	ImGui::PopStyleVar();
	plot->setIsHovered(ImGui::IsItemHovered());
}

void Gui::handleDragRect(uint32_t id, Plot::DragRect& dragRect, ImPlotRect plotLimits)
{
	if (dragRect.getState())
	{
		auto markerPosX0 = dragRect.getValueX0();
		auto markerPosX1 = dragRect.getValueX1();

		if (markerPosX0 == 0.0 && markerPosX1 == 0.0)
		{
			float offset = (std::abs(plotLimits.X.Max) - std::abs(plotLimits.X.Min)) / 3.0f;
			markerPosX0 = plotLimits.X.Min + offset;
			markerPosX1 = plotLimits.X.Min + 2.0f * offset;
			dragRect.setValueX0(markerPosX0);
			dragRect.setValueX1(markerPosX1);
		}

		static ImPlotRect rect(0.0025, 0.0045, 0, 0.5);
		ImPlot::DragRect(id, &markerPosX0, &plotLimits.Y.Min, &markerPosX1, &plotLimits.Y.Max, GuiHelper::dragSelection(), ImPlotDragToolFlags_NoFit);
		dragRect.setValueX0(markerPosX0);
		dragRect.setValueX1(markerPosX1);
	}
	else
	{
		dragRect.setValueX0(0.0);
		dragRect.setValueX1(0.0);
	}
}