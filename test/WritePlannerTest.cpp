#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "WritePlanner.hpp"

/*
 * The write planner is a plain model, so everything here is a statement about
 * what a plan means rather than about what a window draws.
 */

TEST(WritePlannerTest, aNewPlannerIsEmpty)
{
	WritePlanner planner;

	EXPECT_EQ(planner.size(), 0u);
	EXPECT_TRUE(planner.getNames().empty());
	EXPECT_FALSE(planner.hasPlan("plan0"));
}

TEST(WritePlannerTest, freeNameSkipsTheNamesInUse)
{
	WritePlanner planner;

	EXPECT_EQ(WritePlanner::freeName(planner, "plan"), "plan0");

	ASSERT_TRUE(planner.addPlan("plan0"));
	EXPECT_EQ(WritePlanner::freeName(planner, "plan"), "plan1");

	ASSERT_TRUE(planner.addPlan("plan2"));
	EXPECT_EQ(WritePlanner::freeName(planner, "plan"), "plan1");
}

TEST(WritePlannerTest, aPlanIsRefusedWhenTheNameIsTakenOrEmpty)
{
	WritePlanner planner;

	ASSERT_TRUE(planner.addPlan("ramp", "duty"));
	EXPECT_FALSE(planner.addPlan("ramp"));
	EXPECT_FALSE(planner.addPlan(""));
	EXPECT_EQ(planner.size(), 1u);
}

TEST(WritePlannerTest, renameIsRefusedWhenTheNewNameIsTaken)
{
	WritePlanner planner;

	ASSERT_TRUE(planner.addPlan("first"));
	ASSERT_TRUE(planner.addPlan("second"));

	EXPECT_FALSE(planner.renamePlan("first", "second"));
	EXPECT_FALSE(planner.renamePlan("first", ""));
	EXPECT_FALSE(planner.renamePlan("missing", "third"));

	/* The plan keeps its name when the rename is refused, so a refused edit
	   cannot leave two plans under the same name. */
	EXPECT_TRUE(planner.hasPlan("first"));
	EXPECT_EQ(planner.getPlan("first").name, "first");

	ASSERT_TRUE(planner.renamePlan("first", "third"));
	EXPECT_TRUE(planner.hasPlan("third"));
	EXPECT_FALSE(planner.hasPlan("first"));
}

TEST(WritePlannerTest, removingAPlanLeavesTheOthersInPlace)
{
	WritePlanner planner;

	ASSERT_TRUE(planner.addPlan("a"));
	ASSERT_TRUE(planner.addPlan("b"));
	ASSERT_TRUE(planner.addPlan("c"));

	ASSERT_TRUE(planner.removePlan("b"));
	EXPECT_FALSE(planner.removePlan("b"));

	const std::vector<std::string> names = planner.getNames();
	ASSERT_EQ(names.size(), 2u);
	EXPECT_EQ(names[0], "a");
	EXPECT_EQ(names[1], "c");
}

TEST(WritePlannerTest, aStepIsHeldUntilTheNextOneBegins)
{
	WritePlanner planner;
	ASSERT_TRUE(planner.addPlan("ramp"));

	ASSERT_TRUE(planner.addStep("ramp", 0.0, 10.0));
	ASSERT_TRUE(planner.addStep("ramp", 2.0, 20.0));

	const WritePlanner::Plan plan = planner.getPlan("ramp");

	/* Before the first step the first value already applies, which is what makes
	   a plan start at a known level instead of at zero. */
	EXPECT_DOUBLE_EQ(WritePlanner::valueAt(plan, -1.0), 10.0);
	EXPECT_DOUBLE_EQ(WritePlanner::valueAt(plan, 0.0), 10.0);
	EXPECT_DOUBLE_EQ(WritePlanner::valueAt(plan, 1.999), 10.0);
	EXPECT_DOUBLE_EQ(WritePlanner::valueAt(plan, 2.0), 20.0);
	EXPECT_DOUBLE_EQ(WritePlanner::valueAt(plan, 99.0), 20.0);
}

TEST(WritePlannerTest, theLaterOfTwoStepsAtTheSameTimeWins)
{
	WritePlanner planner;
	ASSERT_TRUE(planner.addPlan("ramp"));

	ASSERT_TRUE(planner.addStep("ramp", 1.0, 1.0));
	ASSERT_TRUE(planner.addStep("ramp", 1.0, 2.0));

	EXPECT_DOUBLE_EQ(WritePlanner::valueAt(planner.getPlan("ramp"), 1.0), 2.0);
}

TEST(WritePlannerTest, aPlanWithoutStepsReadsZeroThroughout)
{
	WritePlanner planner;
	ASSERT_TRUE(planner.addPlan("empty"));

	const WritePlanner::Plan plan = planner.getPlan("empty");

	EXPECT_DOUBLE_EQ(WritePlanner::valueAt(plan, 0.0), 0.0);
	EXPECT_DOUBLE_EQ(WritePlanner::valueAt(plan, 5.0), 0.0);
	EXPECT_DOUBLE_EQ(WritePlanner::duration(plan), 0.0);
}

TEST(WritePlannerTest, durationIsTheLastStepInTime)
{
	WritePlanner planner;
	ASSERT_TRUE(planner.addPlan("ramp"));

	ASSERT_TRUE(planner.addStep("ramp", 3.0, 1.0));
	ASSERT_TRUE(planner.addStep("ramp", 7.5, 2.0));
	ASSERT_TRUE(planner.addStep("ramp", 1.0, 3.0));

	/* The list order is the order the user typed, so the longest time has to be
	   looked for rather than read off the last entry. */
	EXPECT_DOUBLE_EQ(WritePlanner::duration(planner.getPlan("ramp")), 7.5);
}

TEST(WritePlannerTest, editingAStepListKeepsTheOtherFields)
{
	WritePlanner planner;
	ASSERT_TRUE(planner.addPlan("ramp", "duty"));
	ASSERT_TRUE(planner.setVariable("ramp", "pwm"));

	std::vector<WritePlanner::Step> steps{{0.0, 0.0}, {1.0, 50.0}, {2.0, 100.0}};
	ASSERT_TRUE(planner.setSteps("ramp", steps));

	ASSERT_TRUE(planner.removeStep("ramp", 1));
	EXPECT_FALSE(planner.removeStep("ramp", 17));

	const WritePlanner::Plan plan = planner.getPlan("ramp");
	ASSERT_EQ(plan.steps.size(), 2u);
	EXPECT_DOUBLE_EQ(plan.steps[0].time, 0.0);
	EXPECT_DOUBLE_EQ(plan.steps[1].value, 100.0);
	EXPECT_EQ(plan.variable, "pwm");
}

TEST(WritePlannerTest, aStepListIsTruncatedRatherThanRefused)
{
	WritePlanner planner;
	ASSERT_TRUE(planner.addPlan("long"));

	std::vector<WritePlanner::Step> steps;

	for (size_t index = 0; index < WritePlanner::maximumStepsPerPlan + 100; index++)
		steps.push_back(WritePlanner::Step{static_cast<double>(index), static_cast<double>(index)});

	/* A hand written project file should still open, so the bound trims the list
	   instead of throwing the whole plan away. */
	ASSERT_TRUE(planner.setSteps("long", steps));
	EXPECT_EQ(planner.getPlan("long").steps.size(), WritePlanner::maximumStepsPerPlan);

	EXPECT_FALSE(planner.addStep("long", 1.0, 1.0));
}

TEST(WritePlannerTest, editingAMissingPlanDoesNothing)
{
	WritePlanner planner;

	EXPECT_FALSE(planner.setVariable("missing", "duty"));
	EXPECT_FALSE(planner.addStep("missing", 0.0, 1.0));
	EXPECT_FALSE(planner.removeStep("missing", 0));
	EXPECT_FALSE(planner.setSteps("missing", {}));
	EXPECT_FALSE(planner.removePlan("missing"));
	EXPECT_EQ(planner.size(), 0u);
}

TEST(WritePlannerTest, clearRemovesEveryPlan)
{
	WritePlanner planner;

	ASSERT_TRUE(planner.addPlan("a"));
	ASSERT_TRUE(planner.addPlan("b"));
	ASSERT_TRUE(planner.addStep("a", 0.0, 1.0));

	planner.clear();

	EXPECT_EQ(planner.size(), 0u);
	EXPECT_FALSE(planner.hasPlan("a"));
}
