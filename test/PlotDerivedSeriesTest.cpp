#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "Plot.hpp"
#include "Variable.hpp"

/*
 * A series has to know whether its variable is one of the target's or one
 * worked out on this side.
 *
 * Every path that adds a series goes through `Plot::addSeries`, and none of
 * them passes the answer in: it is read off the variable. A path that forgot
 * to say so - a tool call, a project being loaded - would otherwise put a
 * derived variable's made up address into the sampling loop's read plan, and
 * the loop would read whatever lives at that address and present it as the
 * series' value.
 *
 * That failure is worse than a missing curve: what comes back is a number, and
 * a number that should not be there looks exactly like one that should. It
 * was found on a real target, where a queue depth curve came out as a flat
 * line at 8000000 - the contents of whatever memory address zero happened to
 * reach.
 */
TEST(PlotDerivedSeries, ASeriesTakesItsProvenanceFromTheVariable)
{
	Plot plot("measure");

	Variable fromTarget("g_led_ticks", Variable::Type::U32, 0.0);
	Variable workedOutHere("q_sensor[depth]", Variable::Type::U32, 0.0);

	workedOutHere.setDerived(true);

	ASSERT_TRUE(plot.addSeries(&fromTarget));
	ASSERT_TRUE(plot.addSeries(&workedOutHere));

	EXPECT_FALSE(plot.getSeries("g_led_ticks")->derived);
	EXPECT_TRUE(plot.getSeries("q_sensor[depth]")->derived);
}

TEST(PlotDerivedSeries, APlainVariableIsNotDerivedByDefault)
{
	Variable variable("g_led_ticks", Variable::Type::U32, 0.0);

	/* A variable that was never marked is one the target was read for. An
	   answer of "derived" here would leave real variables out of the read
	   plan, which is the same mistake pointing the other way. */
	EXPECT_FALSE(variable.isDerived());
}
