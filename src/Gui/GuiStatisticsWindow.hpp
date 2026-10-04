#pragma once

#include "GuiHelper.hpp"
#include "Plot.hpp"
#include "Statistics.hpp"

/* The measurements window.

   Its visibility belongs to the plot, and the row in the panels is the
   switch for it - so the window can be opened before a series has been
   chosen. Closing the window has to turn that switch off as well, or the row
   would keep claiming something is open that is not. */
class StatisticsWindow
{
   public:
	void drawAnalog(std::shared_ptr<Plot> plt, bool suspended = false)
	{
		if (!beginWindow(plt, suspended))
			return;

		if (auto ser = drawPicker(plt); ser != nullptr)
			drawAnalogRows(plt, ser);

		ImGui::End();
	}

	void drawDigital(std::shared_ptr<Plot> plt, bool suspended = false)
	{
		if (!beginWindow(plt, suspended))
			return;

		if (auto ser = drawPicker(plt); ser != nullptr)
			drawDigitalRows(plt, ser);

		ImGui::End();
	}

   private:
	/* The series picker, its colour swatch and the range switch.

	   Returns the chosen series, or nothing when none is chosen - in which
	   case the switch is cleared, because a range with nothing to measure
	   would keep shading a strip of the plot for no reason.

	   The name is not repeated beside the swatch: the combo already says which
	   series this is, and all the swatch carries is the colour the readings and
	   the shaded range are drawn in. */
	std::shared_ptr<Plot::Series> drawPicker(std::shared_ptr<Plot>& plt)
	{
		std::vector<std::string> serNames{"OFF"};
		for (auto& [name, ser] : plt->getSeriesMap())
			serNames.push_back(name);

		/* The combo takes what is left once the swatch has its room. The swatch
		   is one frame high and sits on the same line, so the width is worked
		   out rather than left to the layout - a combo that fills the line
		   would push the swatch onto the next one. */
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
		ImGui::Combo("##stats", &plt->statisticsSeries, serNames);

		if (plt->statisticsSeries <= 0 || plt->statisticsSeries >= static_cast<int32_t>(serNames.size()))
		{
			plt->statisticsSeries = 0;
			plt->stats.setState(false);
			return nullptr;
		}

		auto ser = plt->getSeries(serNames[plt->statisticsSeries]);

		ImGui::SameLine();
		ImGui::ColorEdit4("##color", &ser->var->getColor().r, ImGuiColorEditFlags_NoInputs);

		static bool selectRange = false;
		ImGui::Text("Select range:");
		ImGui::SameLine();
		ImGui::Checkbox("##selectrange", &selectRange);

		plt->stats.setState(selectRange);

		return ser;
	}

	/* One row of the measurement table: the label in the left column, the value
	   in the right.

	   The two columns exist so the values line up on their own. The row macro
	   used before padded each label with spaces until the values met, which
	   only holds while every label has been counted correctly, and stops
	   holding the moment one of them grows by a character. */
	void drawRow(const char* label, const std::string& value)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Text("%s", label);
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%s", value.c_str());
	}

	/* The two ends of the range are placed with the pointer, so they are known
	   to the resolution the pointer can reach and no further - writing them to
	   six places would suggest a precision that is not there. The measurements
	   between them are computed from the samples, so they get the extra
	   places. */
	void drawTimeRow(const char* label, double seconds)
	{
		drawRow(label, GuiHelper::formatNumber(seconds, " s", 3));
	}

	void drawValueRow(const char* label, double value)
	{
		drawRow(label, GuiHelper::formatNumber(value, "", 6));
	}

	void drawAnalogRows(std::shared_ptr<Plot>& plt, const std::shared_ptr<Plot::Series>& ser)
	{
		Statistics::AnalogResults results;
		Statistics::calculateResults(ser.get(), plt->getXAxisSeries(), plt->stats.getValueX0(), plt->stats.getValueX1(), results);

		if (!ImGui::BeginTable("StatsTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
			return;

		drawTimeRow("t0:", plt->stats.getValueX0());
		drawTimeRow("t1:", plt->stats.getValueX1());
		drawTimeRow("t1 - t0:", plt->stats.getValueX1() - plt->stats.getValueX0());
		drawValueRow("min:", results.min);
		drawValueRow("max:", results.max);
		drawValueRow("delta (max-min):", results.max - results.min);
		drawValueRow("mean:", results.mean);
		drawValueRow("stddev:", results.stddev);

		ImGui::EndTable();
	}

	void drawDigitalRows(std::shared_ptr<Plot>& plt, const std::shared_ptr<Plot::Series>& ser)
	{
		Statistics::DigitalResults results;
		Statistics::calculateResults(ser.get(), plt->getXAxisSeries(), plt->stats.getValueX0(), plt->stats.getValueX1(), results);

		if (!ImGui::BeginTable("DigitalStatsTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
			return;

		drawTimeRow("t0:", plt->stats.getValueX0());
		drawTimeRow("t1:", plt->stats.getValueX1());
		drawTimeRow("t1 - t0:", plt->stats.getValueX1() - plt->stats.getValueX0());
		drawValueRow("Lmin:", results.Lmin);
		drawValueRow("Lmax:", results.Lmax);
		drawValueRow("Hmin:", results.Hmin);
		drawValueRow("Hmax:", results.Hmax);
		drawValueRow("fmin:", results.fmin);
		drawValueRow("fmax:", results.fmax);

		ImGui::EndTable();
	}

	/* Opens the window if the plot has it switched on, and reports whether
	   there is anything to draw into. The close button writes back to the
	   plot, so the switch and the window never disagree.

	   `suspended` is for a trace that is still being written: the row keeps
	   its setting but nothing is drawn, so that the measurement does not come
	   back half empty and the setting is not thrown away either. */
	bool beginWindow(std::shared_ptr<Plot> plt, bool suspended)
	{
		if (suspended || !plt->getStatisticsVisible())
		{
			plt->stats.setState(false);
			return false;
		}

		/* The table splits its width evenly between the two columns, so a window
		   narrower than the widest label cuts that label short - and the window
		   is remembered across runs, so one that was saved at a narrow width
		   before the table existed keeps the clipping for good.

		   The width comes from the text rather than from a fixed number, so it
		   follows the font and the display scale, and it is set as a constraint
		   so that a window already saved too small is pulled back up instead of
		   staying where it is. */
		const ImGuiStyle& style = ImGui::GetStyle();
		const float labelColumn = ImGui::CalcTextSize("delta (max-min):").x + style.CellPadding.x * 2.0f;
		const float minimumWidth = labelColumn * 2.0f + style.WindowPadding.x * 2.0f;
		ImGui::SetNextWindowSizeConstraints(ImVec2(minimumWidth, 0.0f), ImVec2(FLT_MAX, FLT_MAX));

		/* A series index survives in the plot while the series list can
		   change under it, and looking up an index that no longer exists
		   throws rather than returning nothing, so it is put back in range
		   before anything reads it. */
		if (plt->statisticsSeries < 0 || plt->statisticsSeries > static_cast<int32_t>(plt->getSeriesMap().size()))
			plt->statisticsSeries = 0;

		bool open = true;

		/* No collapse arrow, as upstream: the window is either open or closed by
		   its switch, and a rolled-up title bar would be a third state the
		   switch does not know about. */
		ImGui::Begin("Statistics", &open, ImGuiWindowFlags_NoCollapse);

		if (!open)
			plt->setStatisticsVisible(false);

		return true;
	}
};
