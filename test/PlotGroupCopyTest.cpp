#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "Plot.hpp"
#include "PlotGroupHandler.hpp"
#include "PlotHandler.hpp"

/*
 * A group is the unit the user works in: it is what a recorder captures, what
 * the tree menu adds plots to, and what a project file stores. Copying one is
 * therefore offered for the whole group rather than for its parts, from the
 * tree menu and from the MCP tool behind it.
 *
 * The awkward part of a copy is the branch. A group may hold groups, and the
 * children name their parent, so a copy that duplicated only the top group
 * would leave its children behind pointing at the original - the copy would
 * come out empty and the original would gain a second set of children. The
 * whole branch therefore travels as a unit, and every group below the copy is
 * created under the copy.
 *
 * A second awkward part is the type. The same call serves both "copy this
 * group" and "copy it into a group of the other kind", so the type is asked
 * for by the caller rather than taken from the original.
 */

namespace
{
	std::vector<std::string> plotNamesOf(const PlotGroup& group)
	{
		std::vector<std::string> names;

		for (const auto& [name, entry] : group)
			names.push_back(name);

		return names;
	}
}

TEST(PlotGroupNamingTest, aFreshGroupNameTakesTheStemAndTheNumberZero)
{
	PlotGroupHandler handler;

	EXPECT_EQ(handler.makeFreeName("new sampling group"), "new sampling group0");
	EXPECT_EQ(handler.makeFreeName("new recorder group"), "new recorder group0");
}

TEST(PlotGroupNamingTest, aGroupNameInUsePushesTheNumberUp)
{
	PlotGroupHandler handler;
	handler.addGroup("new sampling group0");
	handler.addGroup("new sampling group1");

	EXPECT_EQ(handler.makeFreeName("new sampling group"), "new sampling group2");
}

TEST(PlotGroupNamingTest, aCopiedGroupIsNamedAfterTheGroupItCameFrom)
{
	PlotGroupHandler handler;
	handler.addGroup("wall panel");

	EXPECT_EQ(handler.makeFreeCopyName("wall panel"), "wall panel_copy_0");

	handler.addGroup("wall panel_copy_0");

	EXPECT_EQ(handler.makeFreeCopyName("wall panel"), "wall panel_copy_1");
}

TEST(PlotGroupCopyTest, aCopyCarriesThePlotsOfTheOriginal)
{
	PlotHandler plotHandler;
	PlotGroupHandler groupHandler;

	auto group = groupHandler.addGroup("wall panel");
	auto curve = plotHandler.addPlot("curve");
	auto table = plotHandler.addPlot("table");

	group->addPlot(curve);
	group->addPlot(table, false);

	const std::string copyName = groupHandler.copyGroup("wall panel", PlotGroup::Type::Sampling, plotHandler);

	EXPECT_EQ(copyName, "wall panel_copy_0");
	ASSERT_TRUE(groupHandler.checkIfGroupExists(copyName));

	auto copy = groupHandler.getGroup(copyName);

	/* The copies land in the same places, and the one that was hidden in the
	   original is hidden in the copy as well. */
	auto names = plotNamesOf(*copy);
	std::sort(names.begin(), names.end());
	EXPECT_EQ(names, (std::vector<std::string>{"curve_copy_0", "table_copy_0"}));
	EXPECT_FALSE(copy->getVisibility("table_copy_0"));

	/* The plots themselves are registered with the plot handler, so the
	   drawing code will find them by name. */
	EXPECT_TRUE(plotHandler.checkIfPlotExists("curve_copy_0"));
	EXPECT_TRUE(plotHandler.checkIfPlotExists("table_copy_0"));
	EXPECT_EQ(plotHandler.getPlotsCount(), 4u);
}

TEST(PlotGroupCopyTest, aCopyCarriesTheWholeBranchBelowIt)
{
	PlotHandler plotHandler;
	PlotGroupHandler groupHandler;

	groupHandler.addGroup("root");
	groupHandler.addGroup("child", PlotGroup::Type::Sampling, "root");
	groupHandler.addGroup("leaf", PlotGroup::Type::Sampling, "child");

	const std::string copyName = groupHandler.copyGroup("root", PlotGroup::Type::Sampling, plotHandler);

	ASSERT_EQ(copyName, "root_copy_0");
	ASSERT_EQ(groupHandler.getGroupCount(), 6u);

	/* Every group below the copy is created under the copy, not left hanging
	   under the original. */
	ASSERT_TRUE(groupHandler.checkIfGroupExists("child_copy_0"));
	ASSERT_TRUE(groupHandler.checkIfGroupExists("leaf_copy_0"));

	EXPECT_EQ(groupHandler.getGroup("child_copy_0")->getParentName(), copyName);
	EXPECT_EQ(groupHandler.getGroup("leaf_copy_0")->getParentName(), "child_copy_0");

	/* The original keeps its own branch exactly as it was. */
	EXPECT_EQ(groupHandler.getGroup("child")->getParentName(), "root");
	EXPECT_EQ(groupHandler.getGroup("leaf")->getParentName(), "child");
}

TEST(PlotGroupCopyTest, aCopySitsAtTheTopLevelEvenWhenTheOriginalIsNested)
{
	PlotHandler plotHandler;
	PlotGroupHandler groupHandler;

	groupHandler.addGroup("outer");
	groupHandler.addGroup("inner", PlotGroup::Type::Sampling, "outer");

	const std::string copyName = groupHandler.copyGroup("inner", PlotGroup::Type::Sampling, plotHandler);

	/* An empty parent name is what puts a group at the top level, so the copy
	   is reachable without going through the group the original sat in. */
	EXPECT_EQ(copyName, "inner_copy_0");
	EXPECT_EQ(groupHandler.getGroup(copyName)->getParentName(), "");
	EXPECT_TRUE(groupHandler.getGroup(copyName)->isRoot());
}

TEST(PlotGroupCopyTest, aCopyCanBeOfTheOtherKindOfGroup)
{
	PlotHandler plotHandler;
	PlotGroupHandler groupHandler;

	groupHandler.addGroup("wall panel", PlotGroup::Type::Sampling);
	groupHandler.addGroup("child", PlotGroup::Type::Sampling, "wall panel");

	/* This is the "copy it into a recorder group" route: the group that ends
	   up on top is the kind the caller asked for, while the groups nested in
	   it keep the kind they were created with. */
	const std::string copyName = groupHandler.copyGroup("wall panel", PlotGroup::Type::Recorder, plotHandler);

	ASSERT_EQ(copyName, "wall panel_copy_0");
	EXPECT_EQ(groupHandler.getGroup(copyName)->getType(), PlotGroup::Type::Recorder);
	EXPECT_EQ(groupHandler.getGroup(copyName)->getTypeName(), "recorder");
	EXPECT_EQ(groupHandler.getGroup("child_copy_0")->getType(), PlotGroup::Type::Sampling);
}

TEST(PlotGroupCopyTest, copyingAGroupThatIsNotThereGivesNothing)
{
	PlotHandler plotHandler;
	PlotGroupHandler groupHandler;

	groupHandler.addGroup("wall panel");

	EXPECT_EQ(groupHandler.copyGroup("nowhere", PlotGroup::Type::Sampling, plotHandler), "");
	EXPECT_EQ(groupHandler.getGroupCount(), 1u);
}

TEST(PlotGroupCopyTest, copyingAGroupTwiceGivesTwoDistinctNames)
{
	PlotHandler plotHandler;
	PlotGroupHandler groupHandler;

	groupHandler.addGroup("wall panel");

	const std::string first = groupHandler.copyGroup("wall panel", PlotGroup::Type::Sampling, plotHandler);
	const std::string second = groupHandler.copyGroup("wall panel", PlotGroup::Type::Sampling, plotHandler);

	EXPECT_EQ(first, "wall panel_copy_0");
	EXPECT_EQ(second, "wall panel_copy_1");
	EXPECT_EQ(groupHandler.getGroupCount(), 3u);
}

TEST(PlotGroupCopyTest, aChildNestedTwiceBelowIsCopiedToo)
{
	PlotHandler plotHandler;
	PlotGroupHandler groupHandler;

	/* Two groups with the same name can only exist when one of them was copied,
	   so a copy of a copy has to keep numbering from where the branch is now. */
	groupHandler.addGroup("root");
	groupHandler.addGroup("root_copy_0", PlotGroup::Type::Sampling, "root");

	const std::string copyName = groupHandler.copyGroup("root", PlotGroup::Type::Sampling, plotHandler);

	EXPECT_EQ(copyName, "root_copy_1");

	/* The nested group is called root_copy_0, and that name is taken at the
	   top level, so the copy inside the copy gets the next free number. */
	ASSERT_TRUE(groupHandler.checkIfGroupExists("root_copy_0_copy_0"));
	EXPECT_EQ(groupHandler.getGroup("root_copy_0_copy_0")->getParentName(), copyName);
}
