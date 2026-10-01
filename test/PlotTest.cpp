#include <gtest/gtest.h>

#include <string>

#include "Plot.hpp"
#include "Variable.hpp"

/*
 * An axis label is a setting the user types in, and it starts out empty. What
 * an empty label means is therefore the whole of the behaviour: the axis has
 * to keep the name it carried before labels could be set, so that a project
 * written by an older build reads back looking exactly as it was saved.
 *
 * The tests below pin that automatic name down for each kind of plot, and then
 * check that a typed label replaces it without disturbing the others.
 *
 * The vertical axis is named on every kind of plot that has one. That is not
 * only cosmetic: the right click menu of an axis offers a switch to hide its
 * label, and ImPlot greys that switch out while the axis carries no text at
 * all, so an unnamed axis is an axis whose label cannot be turned off.
 */

TEST(PlotAxisLabelTest, aCurveIsDrawnAgainstTime)
{
	Plot plot("p");

	EXPECT_EQ(plot.getDefaultXAxisLabel(), "time[s]");
	EXPECT_EQ(plot.getDefaultYAxisLabel(), "Value");

	/* Nothing was typed in, so the automatic name is what gets drawn. */
	EXPECT_EQ(plot.getEffectiveXAxisLabel(), "time[s]");
	EXPECT_EQ(plot.getEffectiveYAxisLabel(), "Value");
}

TEST(PlotAxisLabelTest, aBarChartNamesTheAxisItsValuesSitOn)
{
	Plot plot("p");
	plot.setType(Plot::Type::BAR);

	/* The series names are the ticks along the bottom, so no axis label is
	   drawn there unless one is asked for. */
	EXPECT_EQ(plot.getDefaultXAxisLabel(), "");
	EXPECT_EQ(plot.getDefaultYAxisLabel(), "Value");
}

TEST(PlotAxisLabelTest, anXYPlotNamesTheHorizontalAxisAfterItsVariable)
{
	Plot plot("p");
	plot.setType(Plot::Type::XY);

	/* Nothing selected yet, so there is nothing to name the axis after. */
	EXPECT_EQ(plot.getDefaultXAxisLabel(), "");

	Variable speed("speed");
	plot.setXAxisVariable(&speed);

	EXPECT_EQ(plot.getDefaultXAxisLabel(), "speed");
}

TEST(PlotAxisLabelTest, everyKindOfPlotWithAVerticalAxisNamesIt)
{
	for (const Plot::Type kind : {Plot::Type::CURVE, Plot::Type::BAR, Plot::Type::XY})
	{
		Plot plot("p");
		plot.setType(kind);

		EXPECT_EQ(plot.getDefaultYAxisLabel(), "Value") << "plot kind " << static_cast<int>(kind);
	}
}

TEST(PlotAxisLabelTest, aTableHasNoAxesToName)
{
	Plot plot("p");
	plot.setType(Plot::Type::TABLE);

	EXPECT_EQ(plot.getDefaultXAxisLabel(), "");
	EXPECT_EQ(plot.getDefaultYAxisLabel(), "");
}

TEST(PlotAxisLabelTest, aTypedLabelReplacesTheAutomaticOne)
{
	Plot plot("p");

	plot.setXAxisLabel("elapsed");
	plot.setYAxisLabel("torque");

	EXPECT_EQ(plot.getXAxisLabel(), "elapsed");
	EXPECT_EQ(plot.getYAxisLabel(), "torque");
	EXPECT_EQ(plot.getEffectiveXAxisLabel(), "elapsed");
	EXPECT_EQ(plot.getEffectiveYAxisLabel(), "torque");
}

TEST(PlotAxisLabelTest, anEmptiedLabelGoesBackToTheAutomaticOne)
{
	Plot plot("p");
	plot.setXAxisLabel("elapsed");

	plot.setXAxisLabel("");

	EXPECT_EQ(plot.getXAxisLabel(), "");
	EXPECT_EQ(plot.getEffectiveXAxisLabel(), "time[s]");
}

TEST(PlotAxisLabelTest, aTypedLabelSurvivesAChangeOfPlotKind)
{
	/* A typed label belongs to the plot rather than to the kind of plot, and
	   the automatic one belongs to the kind. Changing the kind therefore
	   changes only what was not typed in. */
	Plot plot("p");
	plot.setType(Plot::Type::BAR);
	plot.setXAxisLabel("channel");

	EXPECT_EQ(plot.getEffectiveXAxisLabel(), "channel");
	EXPECT_EQ(plot.getEffectiveYAxisLabel(), "Value");

	plot.setType(Plot::Type::CURVE);

	EXPECT_EQ(plot.getEffectiveXAxisLabel(), "channel");
	EXPECT_EQ(plot.getEffectiveYAxisLabel(), "");
}

TEST(PlotAxisLabelTest, changingTheHorizontalVariableChangesTheAutomaticLabel)
{
	Plot plot("p");
	plot.setType(Plot::Type::XY);

	Variable speed("speed");
	Variable rpm("rpm");

	plot.setXAxisVariable(&speed);
	EXPECT_EQ(plot.getEffectiveXAxisLabel(), "speed");

	plot.setXAxisVariable(&rpm);
	EXPECT_EQ(plot.getEffectiveXAxisLabel(), "rpm");

	/* And a typed label no longer follows the variable. */
	plot.setXAxisLabel("shaft");
	plot.setXAxisVariable(&speed);
	EXPECT_EQ(plot.getEffectiveXAxisLabel(), "shaft");
}
