#include <gtest/gtest.h>

#include <cstdint>

#include "Plot.hpp"
#include "PlotHandler.hpp"
#include "Variable.hpp"

/*
 * How long a run can be read back is the number of samples a series keeps, and
 * that number is not capped by the code - what caps it is the memory the
 * buffers are allowed. The rule lives on the plot handler because that is the
 * object that knows how many buffers there are: one for the horizontal axis of
 * every plot, and one for every series drawn on it, each of them holding every
 * sample twice, once in the ring and once in the copy the drawing reads.
 *
 * The tests below pin the arithmetic and both ends of it: a capacity that fits
 * is the capacity allocated, and one that does not is cut down rather than
 * trusted.
 */

namespace
{
	constexpr uint64_t budget = PlotHandler::memoryBudgetBytes;
	constexpr size_t pointBytes = PlotHandler::bytesPerBufferedPoint;
}

TEST(PlotHandlerMemoryBudgetTest, theCeilingIsWhatTheBuffersCanAfford)
{
	PlotHandler handler;

	/* Nothing to fill yet, so the whole budget is free. */
	EXPECT_EQ(handler.countBuffers(), 0u);
	EXPECT_EQ(handler.maxPointsForMemoryBudget(), budget / pointBytes);

	auto plot = handler.addPlot("p");

	/* A plot's horizontal axis is a buffer of its own. */
	EXPECT_EQ(handler.countBuffers(), 1u);

	Variable first("first");
	Variable second("second");
	plot->addSeries(&first);
	plot->addSeries(&second);

	EXPECT_EQ(handler.countBuffers(), 3u);
	EXPECT_EQ(handler.maxPointsForMemoryBudget(), budget / (pointBytes * 3));
}

TEST(PlotHandlerMemoryBudgetTest, everyBufferOfEveryPlotFollowsTheSetting)
{
	PlotHandler handler;

	auto one = handler.addPlot("one");
	auto two = handler.addPlot("two");

	Variable speed("speed");
	one->addSeries(&speed);

	handler.setMaxPoints(25000);

	EXPECT_EQ(one->getXAxisSeries()->getMaxSize(), 25000u);
	EXPECT_EQ(one->getSeriesMap().at("speed")->buffer->getMaxSize(), 25000u);

	/* A plot nobody has drawn on yet is set up the same way, so that adding a
	   series to it later starts from the capacity in use. */
	EXPECT_EQ(two->getXAxisSeries()->getMaxSize(), 25000u);
}

TEST(PlotHandlerMemoryBudgetTest, aCapacityBeyondTheCeilingIsCutToIt)
{
	PlotHandler handler;

	auto plot = handler.addPlot("p");

	Variable speed("speed");
	plot->addSeries(&speed);

	const uint32_t ceiling = handler.maxPointsForMemoryBudget();

	handler.setMaxPoints(ceiling + 1000000);

	EXPECT_EQ(plot->getXAxisSeries()->getMaxSize(), ceiling);
	EXPECT_EQ(plot->getSeriesMap().at("speed")->buffer->getMaxSize(), ceiling);
}

TEST(PlotHandlerMemoryBudgetTest, theCapacityInUseIsNotDisturbedByTheSameRequest)
{
	PlotHandler handler;

	auto plot = handler.addPlot("p");

	Variable speed("speed");
	plot->addSeries(&speed);

	handler.setMaxPoints(3000);
	plot->getXAxisSeries()->addPoint(1.0);
	plot->getSeriesMap().at("speed")->buffer->addPoint(2.0);

	/* Settings are applied on every frame the panel showing them is drawn, so
	   asking again for what is already set has to leave the samples alone. */
	handler.setMaxPoints(3000);

	EXPECT_EQ(plot->getXAxisSeries()->getSize(), 1u);
	EXPECT_EQ(plot->getSeriesMap().at("speed")->buffer->getSize(), 1u);
}
