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
	EXPECT_EQ(plot.getEffectiveYAxisLabel(), "Value");
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

/*
 * The cursors are the two pairs of lines a stopped plot is measured with. Two
 * switches decide what is drawn and they answer different questions: the first
 * says whether to measure at all, the second says which directions to measure
 * in. Keeping them apart is what lets the mode mean something while the
 * cursors are off, so that turning them on does not silently change direction.
 */

TEST(PlotCursorTest, aPlotStartsWithNoCursors)
{
	Plot plot("p");

	EXPECT_FALSE(plot.getCursorsVisible());
	EXPECT_EQ(plot.getCursorMode(), Plot::CursorMode::X);

	/* Both directions report that they are not drawn, so nothing is drawn
	   even before the mode is looked at. */
	EXPECT_FALSE(plot.drawsXCursors());
	EXPECT_FALSE(plot.drawsYCursors());
}

TEST(PlotCursorTest, theModeSaysWhichDirectionsAreDrawn)
{
	Plot plot("p");
	plot.setCursorsVisible(true);

	plot.setCursorMode(Plot::CursorMode::X);
	EXPECT_TRUE(plot.drawsXCursors());
	EXPECT_FALSE(plot.drawsYCursors());

	plot.setCursorMode(Plot::CursorMode::Y);
	EXPECT_FALSE(plot.drawsXCursors());
	EXPECT_TRUE(plot.drawsYCursors());

	plot.setCursorMode(Plot::CursorMode::XY);
	EXPECT_TRUE(plot.drawsXCursors());
	EXPECT_TRUE(plot.drawsYCursors());
}

TEST(PlotCursorTest, switchingTheCursorsOffStopsBothDirections)
{
	for (const Plot::CursorMode mode : {Plot::CursorMode::X, Plot::CursorMode::Y, Plot::CursorMode::XY})
	{
		Plot plot("p");
		plot.setCursorMode(mode);
		plot.setCursorsVisible(true);
		plot.setCursorsVisible(false);

		EXPECT_FALSE(plot.drawsXCursors()) << "mode " << static_cast<int>(mode);
		EXPECT_FALSE(plot.drawsYCursors()) << "mode " << static_cast<int>(mode);
	}
}

TEST(PlotCursorTest, theModeSurvivesBeingSwitchedOff)
{
	/* A user who switched the cursors off keeps the direction they had
	   chosen, so switching them back on draws what they were looking at. */
	Plot plot("p");
	plot.setCursorsVisible(true);
	plot.setCursorMode(Plot::CursorMode::XY);
	plot.setCursorsVisible(false);

	EXPECT_EQ(plot.getCursorMode(), Plot::CursorMode::XY);

	plot.setCursorsVisible(true);
	EXPECT_TRUE(plot.drawsXCursors());
	EXPECT_TRUE(plot.drawsYCursors());
}

TEST(PlotCursorTest, theModeNamesLineUpWithTheModeValues)
{
	/* The combo box is filled from the names and written back through the
	   index, so the two arrays have to agree in order and length. */
	EXPECT_EQ(Plot::cursorModeNames[0], std::string("X"));
	EXPECT_EQ(Plot::cursorModeNames[1], std::string("Y"));
	EXPECT_EQ(Plot::cursorModeNames[2], std::string("X+Y"));

	EXPECT_EQ(Plot::cursorModeToIndex(Plot::CursorMode::X), 0u);
	EXPECT_EQ(Plot::cursorModeToIndex(Plot::CursorMode::Y), 1u);
	EXPECT_EQ(Plot::cursorModeToIndex(Plot::CursorMode::XY), 2u);
}

TEST(PlotCursorTest, aCorruptModeNumberFallsBackToTheFirstMode)
{
	/* The number comes out of a file that may have been written by a newer
	   build or edited by hand, so it is not trusted past the last mode. */
	EXPECT_EQ(Plot::cursorModeFromIndex(0u), Plot::CursorMode::X);
	EXPECT_EQ(Plot::cursorModeFromIndex(1u), Plot::CursorMode::Y);
	EXPECT_EQ(Plot::cursorModeFromIndex(2u), Plot::CursorMode::XY);

	EXPECT_EQ(Plot::cursorModeFromIndex(3u), Plot::CursorMode::X);
	EXPECT_EQ(Plot::cursorModeFromIndex(999u), Plot::CursorMode::X);
}

TEST(PlotCursorTest, bothDirectionsHaveTheirOwnPairOfCursors)
{
	/* A single line can say where something is; only two of them can say how
	   far apart two things are. Each direction therefore carries its own
	   pair, and the four are distinct. */
	Plot plot("p");

	plot.markerX0.setValue(1.0);
	plot.markerX1.setValue(2.0);
	plot.markerY0.setValue(3.0);
	plot.markerY1.setValue(4.0);

	EXPECT_EQ(plot.markerX0.getValue(), 1.0);
	EXPECT_EQ(plot.markerX1.getValue(), 2.0);
	EXPECT_EQ(plot.markerY0.getValue(), 3.0);
	EXPECT_EQ(plot.markerY1.getValue(), 4.0);
}

/*
 * What a cursor reading looks like. The horizontal axis of a curve is drawn
 * in seconds, but the spans it is measured over are the ones between one run
 * of the firmware and the next, so a time cursor reports milliseconds. The
 * rate that goes with the span is the reading the pair is usually opened for,
 * so it is carried alongside the time rather than left to be worked out.
 *
 * The values are strings because that is what reaches the screen, and a test
 * that only checked the numbers would not catch a missing unit.
 */

TEST(CursorReadingTest, aTimeCursorReportsMilliseconds)
{
	EXPECT_EQ(Plot::formatCursorMilliseconds(0.352792), "352.792 ms");
	EXPECT_EQ(Plot::formatCursorMilliseconds(0.719459), "719.459 ms");
	EXPECT_EQ(Plot::formatCursorMilliseconds(0.366667), "366.667 ms");

	/* A whole number of milliseconds keeps no decimal point, and a negative
	   span - which a cursor dragged past its partner produces - keeps its
	   sign so that the reader can see the order they are in. */
	EXPECT_EQ(Plot::formatCursorMilliseconds(0.5), "500 ms");
	EXPECT_EQ(Plot::formatCursorMilliseconds(-0.25), "-250 ms");
	EXPECT_EQ(Plot::formatCursorMilliseconds(0.0), "0 ms");
}

TEST(CursorReadingTest, aSpanIsAlsoReportedAsItsRate)
{
	EXPECT_EQ(Plot::formatCursorRate(0.366667), "2.7");
	EXPECT_EQ(Plot::formatCursorRate(0.01), "100.0");

	/* The rate of a span read the other way round is the same rate, so the
	   sign of the span does not turn into a negative frequency. */
	EXPECT_EQ(Plot::formatCursorRate(-0.366667), "2.7");

	/* A pair sitting on the same instant has no rate to report. Writing
	   infinity there would be a number nobody asked for, so the field is
	   left to say that it has nothing rather than inventing one. */
	EXPECT_EQ(Plot::formatCursorRate(0.0), "-");
}

TEST(CursorReadingTest, aPlainValueDropsTheDigitsThatSayNothing)
{
	/* The vertical axis is not a time, so its readings keep the unit the
	   axis carries and lose only the trailing zeroes. */
	EXPECT_EQ(Plot::formatCursorValue(0.5), "0.5");
	EXPECT_EQ(Plot::formatCursorValue(2.0), "2");
	EXPECT_EQ(Plot::formatCursorValue(-0.033333), "-0.03333");
	EXPECT_EQ(Plot::formatCursorValue(0.0), "0");
}
