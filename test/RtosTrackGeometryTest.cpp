#include <gtest/gtest.h>

#include "RtosTrackGeometry.hpp"

namespace
{
	RtosTrackEntry span(double from, double to, uint8_t depth = 1)
	{
		RtosTrackEntry entry;

		entry.kind = RtosTrackEntry::Kind::Isr;
		entry.hasSeconds = true;
		entry.fromSeconds = from;
		entry.toSeconds = to;
		entry.depth = depth;

		return entry;
	}
}  // namespace

TEST(RtosTrackGeometryTest, ASpanLandsWhereItsTwoEndsSay)
{
	/* A hundred pixels across two seconds: one second is fifty pixels. */
	const std::vector<RtosTrackEntry> entries = {span(0.5, 0.75)};

	const RtosTrackLayout layout = layoutIntervals(entries, 0.0, 2.0, 100.0f, 1.0f);

	ASSERT_EQ(1u, layout.marks.size());
	EXPECT_FLOAT_EQ(25.0f, layout.marks[0].left);
	EXPECT_FLOAT_EQ(37.5f, layout.marks[0].right);
	EXPECT_EQ(1, layout.marks[0].depth);
}

TEST(RtosTrackGeometryTest, AnInterruptShorterThanAPixelIsStillOnePixelWide)
{
	/* Twenty milliseconds across two seconds is one pixel; a twenty microsecond
	   interrupt is a thousandth of that, and rounding it to zero is how a band
	   reads as empty while the target takes an interrupt every twenty
	   milliseconds. */
	const std::vector<RtosTrackEntry> entries = {span(1.0, 1.00002)};

	const RtosTrackLayout layout = layoutIntervals(entries, 0.0, 2.0, 100.0f, 1.0f);

	ASSERT_EQ(1u, layout.marks.size());
	EXPECT_NEAR(1.0f, layout.marks[0].right - layout.marks[0].left, 0.001f);
}

TEST(RtosTrackGeometryTest, ASpanThatCrossesTheLeftEdgeIsClippedNotDropped)
{
	/* It is still holding the CPU at the edge; the picture should say so. */
	const std::vector<RtosTrackEntry> entries = {span(-1.0, 0.5)};

	const RtosTrackLayout layout = layoutIntervals(entries, 0.0, 2.0, 100.0f, 1.0f);

	ASSERT_EQ(1u, layout.marks.size());
	EXPECT_FLOAT_EQ(0.0f, layout.marks[0].left);
	EXPECT_FLOAT_EQ(25.0f, layout.marks[0].right);
	EXPECT_EQ(0u, layout.outside);
}

TEST(RtosTrackGeometryTest, ScatteredSpansAreCountedRatherThanIgnored)
{
	const std::vector<RtosTrackEntry> entries = {span(0.1, 0.2), span(9.0, 9.1), span(-4.0, -3.0)};

	const RtosTrackLayout layout = layoutIntervals(entries, 0.0, 2.0, 100.0f, 1.0f);

	EXPECT_EQ(1u, layout.marks.size());
	EXPECT_EQ(2u, layout.outside);
}

TEST(RtosTrackGeometryTest, ASpanWithoutBothEndsIsNotPlaced)
{
	std::vector<RtosTrackEntry> entries = {span(0.5, 0.6)};

	entries[0].hasSeconds = false;

	const RtosTrackLayout layout = layoutIntervals(entries, 0.0, 2.0, 100.0f, 1.0f);

	EXPECT_TRUE(layout.marks.empty());
	EXPECT_EQ(1u, layout.untimed);
}

TEST(RtosTrackGeometryTest, NestingKeepsItsDepth)
{
	const std::vector<RtosTrackEntry> entries = {span(0.1, 0.9, 1), span(0.2, 0.8, 2)};

	const RtosTrackLayout layout = layoutIntervals(entries, 0.0, 2.0, 100.0f, 1.0f);

	ASSERT_EQ(2u, layout.marks.size());
	EXPECT_EQ(1, layout.marks[0].depth);
	EXPECT_EQ(2, layout.marks[1].depth);
}

TEST(RtosTrackGeometryTest, NoWindowMeansNoMarks)
{
	const std::vector<RtosTrackEntry> entries = {span(0.1, 0.2)};

	EXPECT_TRUE(layoutIntervals(entries, 1.0, 1.0, 100.0f, 1.0f).marks.empty());
	EXPECT_TRUE(layoutIntervals(entries, 0.0, 2.0, 0.0f, 1.0f).marks.empty());
}
