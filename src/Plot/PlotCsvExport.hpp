#pragma once

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "IFileHandler.hpp"
#include "Plot.hpp"
#include "spdlog/spdlog.h"

/* One plot's series written out as a CSV table: a column of timestamps and one
   column per series, in the order the plot holds them.

   It lives outside either panel because both offer it - the plots tree on the
   variable side and the channel panel on the trace side - and the columns have
   to come out the same from both. Keeping one copy is also what keeps them the
   same: two copies would drift the first time a column is added.

   The buffers are copied first, because what is read here is the copy rather
   than the ring the acquisition writes into. A plot that is not on screen has
   not had its copy refreshed since it last was, and the export would otherwise
   hand out whatever was in it then. */
inline void exportPlotToCSV(const std::shared_ptr<Plot>& plot, IFileHandler* fileHandler, spdlog::logger* logger)
{
	const std::string path = fileHandler->saveFile({{"CSV", "csv"}});

	if (path.empty())
		return;

	std::ofstream csvFile(path);

	if (!csvFile)
	{
		logger->info("Error opening the file: {}", path);
		return;
	}

	plot->getXAxisSeries()->copyData();

	for (auto& [name, series] : plot->getSeriesMap())
		series->buffer->copyData();

	const uint32_t size = plot->getXAxisSeries()->getSize();
	const uint32_t offset = plot->getXAxisSeries()->getOffset();

	csvFile << "time [s],";

	for (auto& [name, series] : plot->getSeriesMap())
		csvFile << name << ",";

	csvFile << std::endl;

	for (uint32_t i = 0; i < size; ++i)
	{
		const uint32_t index = (offset + i < size) ? offset + i : i - (size - offset);
		csvFile << plot->getXAxisSeries()->getFirstElementCopy()[index] << ",";

		for (auto& [name, series] : plot->getSeriesMap())
			csvFile << series->buffer->getFirstElementCopy()[index] << ",";

		csvFile << std::endl;
	}

	csvFile.close();
}
