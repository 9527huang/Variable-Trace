#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "Plot.hpp"
#include "ScrollingBuffer.hpp"

class PlotHandler
{
   public:
	std::shared_ptr<Plot> addPlot(const std::string& name);
	bool removePlot(const std::string& name);
	bool renamePlot(const std::string& oldName, const std::string& newName);
	bool removeAllPlots();
	std::shared_ptr<Plot> getPlot(std::string name);
	bool eraseAllPlotData();
	uint32_t getVisiblePlotsCount() const;
	uint32_t getPlotsCount() const;
	bool checkIfPlotExists(const std::string& name) const;
	void setMaxPoints(uint32_t maxPoints);

	/* A name built from the stem that no plot carries yet: the stem itself
	   while it is free, then the stem with the first free number after it.
	   That is how a new plot is named. */
	std::string makeFreeName(const std::string& stem) const;

	/* A name for a copy of a plot, so that a copy is recognisable as one. */
	std::string makeFreeCopyName(const std::string& stem) const;

	/* Adds a plot holding a copy of the configuration of source. The name is
	   built from the stem and is free by construction; which group the plot
	   lands in is the caller's decision. */
	std::shared_ptr<Plot> addPlotCopy(const Plot& source, const std::string& stem);

	class iterator
	{
	   public:
		using iterator_category = std::forward_iterator_tag;
		explicit iterator(std::map<std::string, std::shared_ptr<Plot>>::iterator iter);
		iterator& operator++();
		iterator operator++(int);
		bool operator==(const iterator& other) const;
		bool operator!=(const iterator& other) const;
		std::shared_ptr<Plot> operator*();

	   private:
		std::map<std::string, std::shared_ptr<Plot>>::iterator m_iter;
	};

	iterator begin();
	iterator end();

   protected:
	std::map<std::string, std::shared_ptr<Plot>> plotsMap;
};
