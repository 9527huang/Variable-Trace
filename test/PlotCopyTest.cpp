#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "Plot.hpp"
#include "PlotHandler.hpp"
#include "Variable.hpp"

/*
 * Copying a plot is how a second view of the same signals is made. It is
 * offered from the tree menu and from the MCP tool behind it, so both routes
 * have to produce the same thing.
 *
 * What the copy carries is a promise to the user: the kind of plot, the names
 * on its axes, the variables it shows and the switches that were turned on all
 * come along. The samples that have already run through the original do not -
 * the copy starts its own history the moment it appears, and a copy that
 * arrived with the samples of the original would look like the same run had
 * been recorded twice.
 *
 * These tests pin that split down, and then check the names a copy is given:
 * a copy has to be recognisable as one, and two copies of the same plot must
 * not collide.
 */

TEST(PlotCopyTest, aCopyCarriesTheKindOfPlotAndTheNamesOnItsAxes)
{
	Plot source("speed");
	source.setType(Plot::Type::BAR);
	source.setAlias("motor speed");
	source.setXAxisLabel("t [ms]");
	source.setYAxisLabel("rpm");
	source.setCursorsVisible(true);
	source.setCursorMode(Plot::CursorMode::XY);
	source.setStatisticsVisible(true);
	source.setVisibility(false);
	source.setDomain(Plot::Domain::DIGITAL);
	source.setTraceVarType(Plot::TraceVarType::U16);

	source.markerX0.setValue(1.5);
	source.markerX1.setValue(2.5);
	source.markerY0.setValue(10.0);
	source.markerY1.setValue(20.0);

	Plot copy("speed_copy");
	copy.copyConfigurationFrom(source);

	EXPECT_EQ(copy.getType(), Plot::Type::BAR);
	EXPECT_EQ(copy.getAlias(), "motor speed");
	EXPECT_EQ(copy.getXAxisLabel(), "t [ms]");
	EXPECT_EQ(copy.getYAxisLabel(), "rpm");
	EXPECT_TRUE(copy.getCursorsVisible());
	EXPECT_EQ(copy.getCursorMode(), Plot::CursorMode::XY);
	EXPECT_TRUE(copy.getStatisticsVisible());
	EXPECT_FALSE(copy.getVisibility());
	EXPECT_EQ(copy.getDomain(), Plot::Domain::DIGITAL);
	EXPECT_EQ(copy.getTraceVarType(), Plot::TraceVarType::U16);

	EXPECT_DOUBLE_EQ(copy.markerX0.getValue(), 1.5);
	EXPECT_DOUBLE_EQ(copy.markerX1.getValue(), 2.5);
	EXPECT_DOUBLE_EQ(copy.markerY0.getValue(), 10.0);
	EXPECT_DOUBLE_EQ(copy.markerY1.getValue(), 20.0);
}

TEST(PlotCopyTest, aCopyShowsTheSameVariablesWithoutTheSamplesOfTheOriginal)
{
	Variable speed("speed");
	Variable current("current");

	Plot source("p");
	source.addSeries(&speed);
	source.addSeries(&current);

	/* The original has been running for a while, and one of its traces was
	   switched off and shown in hex. */
	source.getSeries("speed")->buffer->addPoint(1.0);
	source.getSeries("speed")->buffer->addPoint(2.0);
	source.getSeries("current")->visible = false;
	source.getSeries("current")->format = Plot::displayFormat::HEX;

	Plot copy("p_copy");
	copy.copyConfigurationFrom(source);

	ASSERT_EQ(copy.getSeriesMap().size(), 2u);
	EXPECT_TRUE(copy.getSeriesMap().contains("speed"));
	EXPECT_TRUE(copy.getSeriesMap().contains("current"));

	/* The settings travel. */
	EXPECT_FALSE(copy.getSeries("current")->visible);
	EXPECT_EQ(copy.getSeries("current")->format, Plot::displayFormat::HEX);

	/* The buffers do not: the copy owns an empty one of its own, so filling
	   it up cannot disturb the original and the two histories stay apart. */
	EXPECT_NE(copy.getSeries("speed")->buffer.get(), source.getSeries("speed")->buffer.get());
	EXPECT_EQ(source.getSeries("speed")->buffer->getSize(), 2u);
	EXPECT_EQ(copy.getSeries("speed")->buffer->getSize(), 0u);
}

TEST(PlotCopyTest, aCopyOfAnXYPlotKeepsTheVariableAlongTheBottom)
{
	Variable time("time");
	Variable speed("speed");

	Plot source("xy");
	source.setType(Plot::Type::XY);
	source.setXAxisVariable(&time);
	source.addSeries(&speed);

	Plot copy("xy_copy");
	copy.copyConfigurationFrom(source);

	/* Without the variable along the bottom the copy would not know what its
	   samples are plotted against. */
	EXPECT_EQ(copy.getXAxisVariable(), &time);
	EXPECT_EQ(copy.getType(), Plot::Type::XY);
}

TEST(PlotCopyTest, aCopyOfAnEmptyPlotShowsNothing)
{
	Plot source("p");

	Plot copy("p_copy");
	copy.copyConfigurationFrom(source);

	EXPECT_TRUE(copy.getSeriesMap().empty());
}

TEST(PlotCopyNamingTest, aFreshNameTakesTheStemAndTheNumberZero)
{
	PlotHandler handler;

	EXPECT_EQ(handler.makeFreeName("new plot"), "new plot0");
	EXPECT_EQ(handler.makeFreeCopyName("curve"), "curve_copy_0");
}

TEST(PlotCopyNamingTest, aNameInUsePushesTheNumberUp)
{
	PlotHandler handler;
	handler.addPlot("new plot0");
	handler.addPlot("new plot1");
	handler.addPlot("curve_copy_0");

	/* Two plots are already called new plot followed by a number, so the next
	   free one is the one after both of them. */
	EXPECT_EQ(handler.makeFreeName("new plot"), "new plot2");
	EXPECT_EQ(handler.makeFreeCopyName("curve"), "curve_copy_1");
}

TEST(PlotCopyNamingTest, theStemItselfIsNotATakenName)
{
	PlotHandler handler;
	handler.addPlot("curve");

	/* A plot called curve is not a plot called curve followed by a number, so
	   the first copy is still the zero-th one. */
	EXPECT_EQ(handler.makeFreeCopyName("curve"), "curve_copy_0");
}

TEST(PlotCopyNamingTest, copyingAPlotRegistersTheCopyNextToTheOriginal)
{
	Variable speed("speed");

	PlotHandler handler;
	auto source = handler.addPlot("speed");
	source->addSeries(&speed);
	source->setType(Plot::Type::TABLE);

	auto copy = handler.addPlotCopy(*source, source->getName());

	EXPECT_EQ(copy->getName(), "speed_copy_0");
	EXPECT_TRUE(handler.checkIfPlotExists("speed"));
	EXPECT_TRUE(handler.checkIfPlotExists("speed_copy_0"));
	EXPECT_EQ(handler.getPlotsCount(), 2u);

	/* The copy is a copy in every way the user can see, not just in name. */
	EXPECT_EQ(copy->getType(), Plot::Type::TABLE);
	EXPECT_TRUE(copy->getSeriesMap().contains("speed"));
}

TEST(PlotCopyNamingTest, copyingTheSamePlotTwiceGivesTwoDistinctNames)
{
	PlotHandler handler;
	auto source = handler.addPlot("speed");

	auto first = handler.addPlotCopy(*source, source->getName());
	auto second = handler.addPlotCopy(*source, source->getName());

	EXPECT_EQ(first->getName(), "speed_copy_0");
	EXPECT_EQ(second->getName(), "speed_copy_1");
	EXPECT_EQ(handler.getPlotsCount(), 3u);
}
