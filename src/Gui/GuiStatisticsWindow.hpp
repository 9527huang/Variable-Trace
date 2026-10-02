#pragma once

#include "GuiHelper.hpp"
#include "GuiStatisticsWindow.hpp"
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

		std::vector<std::string> serNames{"OFF"};
		for (auto& [name, ser] : plt->getSeriesMap())
			serNames.push_back(name);

		ImGui::Combo("##stats", &plt->statisticsSeries, serNames);

		if (plt->statisticsSeries != 0 && plt->statisticsSeries < static_cast<int32_t>(serNames.size()))
		{
			static bool selectRange = false;
			auto ser = plt->getSeries(serNames[plt->statisticsSeries]);

			ImGui::ColorEdit4("##", &ser->var->getColor().r, ImGuiColorEditFlags_NoInputs);
			ImGui::SameLine();
			ImGui::Text("%s", ser->var->getName().c_str());

			ImGui::Text("select range: ");
			ImGui::SameLine();
			ImGui::Checkbox("##selectrange", &selectRange);

			plt->stats.setState(selectRange);

			Statistics::AnalogResults results;
			Statistics::calculateResults(ser.get(), plt->getXAxisSeries(), plt->stats.getValueX0(), plt->stats.getValueX1(), results);

			GuiHelper::drawDescriptionWithNumber("t0:      ", plt->stats.getValueX0());
			GuiHelper::drawDescriptionWithNumber("t1:      ", plt->stats.getValueX1());
			GuiHelper::drawDescriptionWithNumber("t1-t0:   ", plt->stats.getValueX1() - plt->stats.getValueX0());
			GuiHelper::drawDescriptionWithNumber("min:     ", results.min);
			GuiHelper::drawDescriptionWithNumber("max:     ", results.max);
			GuiHelper::drawDescriptionWithNumber("mean:    ", results.mean);
			GuiHelper::drawDescriptionWithNumber("stddev:  ", results.stddev);
		}
		else
		{
			plt->statisticsSeries = 0;
			plt->stats.setState(false);
		}

		ImGui::End();
	}

	void drawDigital(std::shared_ptr<Plot> plt, bool suspended = false)
	{
		if (!beginWindow(plt, suspended))
			return;

		std::vector<std::string> serNames{"OFF"};
		for (auto& [name, ser] : plt->getSeriesMap())
			serNames.push_back(name);

		ImGui::Combo("##stats", &plt->statisticsSeries, serNames);

		if (plt->statisticsSeries != 0 && plt->statisticsSeries < static_cast<int32_t>(serNames.size()))
		{
			static bool selectRange = false;
			auto ser = plt->getSeries(serNames[plt->statisticsSeries]);

			ImGui::ColorEdit4("##", &ser->var->getColor().r, ImGuiColorEditFlags_NoInputs);
			ImGui::SameLine();
			ImGui::Text("%s", ser->var->getName().c_str());

			ImGui::Text("select range: ");
			ImGui::SameLine();
			ImGui::Checkbox("##selectrange", &selectRange);

			plt->stats.setState(selectRange);

			Statistics::DigitalResults results;
			Statistics::calculateResults(ser.get(), plt->getXAxisSeries(), plt->stats.getValueX0(), plt->stats.getValueX1(), results);

			GuiHelper::drawDescriptionWithNumber("t0:      ", plt->stats.getValueX0());
			GuiHelper::drawDescriptionWithNumber("t1:      ", plt->stats.getValueX1());
			GuiHelper::drawDescriptionWithNumber("t1-t0:   ", plt->stats.getValueX1() - plt->stats.getValueX0());
			GuiHelper::drawDescriptionWithNumber("Lmin:    ", results.Lmin);
			GuiHelper::drawDescriptionWithNumber("Lmax:    ", results.Lmax);
			GuiHelper::drawDescriptionWithNumber("Hmin:    ", results.Hmin);
			GuiHelper::drawDescriptionWithNumber("Hmax:    ", results.Hmax);
			GuiHelper::drawDescriptionWithNumber("fmin:    ", results.fmin);
			GuiHelper::drawDescriptionWithNumber("fmax:    ", results.fmax);
		}
		else
		{
			plt->statisticsSeries = 0;
			plt->stats.setState(false);
		}

		ImGui::End();
	}

   private:
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

		/* A series index survives in the plot while the series list can
		   change under it, and looking up an index that no longer exists
		   throws rather than returning nothing, so it is put back in range
		   before anything reads it. */
		if (plt->statisticsSeries < 0 || plt->statisticsSeries > static_cast<int32_t>(plt->getSeriesMap().size()))
			plt->statisticsSeries = 0;

		bool open = true;
		ImGui::Begin("Statistics", &open);

		if (!open)
			plt->setStatisticsVisible(false);

		return true;
	}
};
