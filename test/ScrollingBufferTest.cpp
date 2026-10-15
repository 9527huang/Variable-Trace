#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <numeric>

#include "ScrollingBuffer.hpp"
#include "DisplayDecimator.hpp"

class ScrollingBufferTest : public ::testing::Test
{
   protected:
	void SetUp() override
	{
		test.setMaxSize(defaultMaxSize);
		std::array<double, defaultMaxSize + 5> data;
		std::iota(data.begin(), data.end(), 1.0);

		for (const auto& element : data)
			test.addPoint(element);

		test.copyData();

		// for (uint32_t i = 0; i < defaultMaxSize; i++)
		// 	std::cout << *(test.getFirstElement() + i) << " ";
	}

	void TearDown() override
	{
	}

	static constexpr uint32_t defaultMaxSize = 20;
	ScrollingBuffer<double> test{};
};

TEST_F(ScrollingBufferTest, testOverflow)
{
	ASSERT_EQ(*(test.getFirstElement()), 21.0);
}

TEST_F(ScrollingBufferTest, testCopy)
{
	for (uint32_t i; i < test.getMaxSize(); i++)
	{
		ASSERT_EQ(*(test.getFirstElement() + i), *(test.getFirstElementCopy() + i));
	}
}

TEST_F(ScrollingBufferTest, testIndexLookup0)
{
	ASSERT_EQ(test.getIndexFromvalue(19), defaultMaxSize - 2);
}

TEST_F(ScrollingBufferTest, testIndexLookup1)
{
	ASSERT_EQ(test.getIndexFromvalue(20), defaultMaxSize - 1);
}

TEST_F(ScrollingBufferTest, testIndexLookup2)
{
	ASSERT_EQ(test.getIndexFromvalue(21), 0);
}

TEST_F(ScrollingBufferTest, testOldestValue)
{
	ASSERT_EQ(test.getOldestValue(), 6);
}

TEST_F(ScrollingBufferTest, testNewestValue)
{
	ASSERT_EQ(test.getNewestValue(), 25);
}

TEST_F(ScrollingBufferTest, testLinearCopy)
{
	auto vec = test.getLinearData(2, 20);
	std::vector<double> result{23, 24, 25, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
	ASSERT_EQ(vec, result);
}

TEST_F(ScrollingBufferTest, testLinearCopy2)
{
	auto vec = test.getLinearData(19, 6);
	std::vector<double> result{20, 21, 22, 23, 24, 25, 6};
	ASSERT_EQ(vec, result);
}

TEST_F(ScrollingBufferTest, testLinearCopy3)
{
	auto vec = test.getLinearData(0, 0);
	std::vector<double> result{21, 22, 23, 24, 25, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
	ASSERT_EQ(vec, result);
}

/*
 * The capacity is a setting, and what is in hand is the newest stretch of the
 * run. So a capacity raised keeps every sample that is already there, one
 * lowered keeps as many as still fit - giving up the oldest, which is the end
 * a running ring drops from - and asking for the capacity in use changes
 * nothing at all.
 *
 * The fixture has pushed 25 samples into a ring of 20, so what it holds is 6
 * through 25, with 6 as the oldest.
 */

TEST_F(ScrollingBufferTest, raisingTheCapacityKeepsEverySample)
{
	test.setMaxSize(defaultMaxSize * 2);

	EXPECT_EQ(test.getSize(), defaultMaxSize);
	EXPECT_EQ(test.getOldestValue(), 6.0);
	EXPECT_EQ(test.getNewestValue(), 25.0);

	/* And the ring carries on from where it was. */
	test.addPoint(26.0);

	EXPECT_EQ(test.getSize(), defaultMaxSize + 1);
	EXPECT_EQ(test.getOldestValue(), 6.0);
	EXPECT_EQ(test.getNewestValue(), 26.0);
}

TEST_F(ScrollingBufferTest, loweringTheCapacityKeepsTheNewestSamples)
{
	test.setMaxSize(5);

	EXPECT_EQ(test.getSize(), 5);
	EXPECT_EQ(test.getOldestValue(), 21.0);
	EXPECT_EQ(test.getNewestValue(), 25.0);

	/* What was given up went from the oldest end, and what is left is in the
	   order it arrived in. */
	test.copyData();

	for (uint32_t i = 0; i < 5; i++)
		EXPECT_EQ(*(test.getFirstElementCopy() + i), static_cast<double>(i + 21));
}

TEST_F(ScrollingBufferTest, askingForTheCapacityInUseChangesNothing)
{
	test.setMaxSize(defaultMaxSize);

	EXPECT_EQ(test.getSize(), defaultMaxSize);
	EXPECT_EQ(test.getOldestValue(), 6.0);
	EXPECT_EQ(test.getNewestValue(), 25.0);
}

TEST_F(ScrollingBufferTest, aRingEmptiedForARunKeepsItsCapacity)
{
	test.setMaxSize(30);
	test.erase();

	EXPECT_EQ(test.getSize(), 0);
	EXPECT_EQ(test.getMaxSize(), 30);

	test.addPoint(1.0);

	EXPECT_EQ(test.getSize(), 1);
	EXPECT_EQ(test.getOldestValue(), 1.0);
	EXPECT_EQ(test.getNewestValue(), 1.0);
}

TEST_F(ScrollingBufferTest, anEmptyRingHandsOutNoNewestSample)
{
	/* getLastElement() hands out a place inside the ring, and a ring with
	   nothing in it has none: it is the one accessor that answers with a null
	   pointer. A caller that reads through it without asking first ends the
	   program - which is what the bar plot did when a variable was dropped
	   into it before the first sample arrived. No other test here covers that,
	   so the contract is written down.

	   getFirstElementCopy() is a different matter: it points into the copy,
	   which is allocated at full size whatever the ring holds, so it is never
	   null and reading a slot that has not been written yet cannot fall off
	   the end. */
	test.erase();
	test.copyData();

	EXPECT_EQ(test.getSize(), 0);
	EXPECT_EQ(test.getLastElement(), nullptr);
	EXPECT_EQ(test.getNewestValue(), 0.0);

	EXPECT_NE(test.getFirstElementCopy(), nullptr);
}

TEST(ScrollingBufferSnapshotTest, CopiesOnlyTheValidPrefixAndReusesAnUnchangedSnapshot)
{
	ScrollingBuffer<double> buffer;
	buffer.setMaxSize(1000);
	buffer.addPoint(1.0);
	buffer.addPoint(2.0);
	buffer.addPoint(3.0);

	EXPECT_TRUE(buffer.copyData());
	EXPECT_EQ(buffer.getCopySize(), 3u);
	EXPECT_EQ(buffer.getCopyOffset(), 0u);
	const uint64_t generation = buffer.getCopyGeneration();
	EXPECT_FALSE(buffer.copyData());
	EXPECT_EQ(buffer.getCopyGeneration(), generation);

	buffer.addPoint(4.0);
	EXPECT_TRUE(buffer.copyData());
	EXPECT_EQ(buffer.getCopySize(), 4u);
	EXPECT_GT(buffer.getCopyGeneration(), generation);
}

TEST(ScrollingBufferSnapshotTest, WrappedSnapshotKeepsLogicalOrderAndCursorLookup)
{
	ScrollingBuffer<double> buffer;
	buffer.setMaxSize(5);
	for (uint32_t value = 1; value <= 7; ++value)
		buffer.addPoint(static_cast<double>(value));

	ASSERT_TRUE(buffer.copyData());
	ASSERT_EQ(buffer.getCopySize(), 5u);
	EXPECT_EQ(buffer.getCopyOffset(), 2u);

	const double* snapshot = buffer.getFirstElementCopy();
	for (uint32_t index = 0; index < buffer.getCopySize(); ++index)
	{
		const uint32_t position = (index + buffer.getCopyOffset()) % buffer.getCopySize();
		EXPECT_EQ(snapshot[position], static_cast<double>(index + 3));
	}
	EXPECT_EQ(snapshot[buffer.getCopyIndexFromValue(5.5)], 5.0);
}

TEST(DisplayDecimatorTest, KeepsVisibleEndpointsAndPixelExtrema)
{
	std::vector<double> x(1000);
	std::vector<double> y(1000, 0.0);
	std::iota(x.begin(), x.end(), 0.0);
	y[500] = 100.0;
	y[501] = -100.0;

	const DisplayPoints points = decimateDisplaySeries(x.data(), y.data(), 1000, 0, 0.0, 999.0, 10);
	ASSERT_FALSE(points.x.empty());
	EXPECT_LE(points.x.size(), 22u);
	EXPECT_EQ(points.x.front(), 0.0);
	EXPECT_EQ(points.x.back(), 999.0);
	EXPECT_NE(std::find(points.y.begin(), points.y.end(), 100.0), points.y.end());
	EXPECT_NE(std::find(points.y.begin(), points.y.end(), -100.0), points.y.end());
}

TEST(DisplayDecimatorTest, UsesOnlyTheVisibleTimeWindow)
{
	std::vector<double> x(100);
	std::vector<double> y(100);
	std::iota(x.begin(), x.end(), 0.0);
	std::iota(y.begin(), y.end(), 100.0);

	const DisplayPoints points = decimateDisplaySeries(x.data(), y.data(), 100, 0, 40.0, 59.0, 5);
	ASSERT_FALSE(points.x.empty());
	EXPECT_EQ(points.x.front(), 40.0);
	EXPECT_EQ(points.x.back(), 59.0);
	for (double value : points.x)
	{
		EXPECT_GE(value, 40.0);
		EXPECT_LE(value, 59.0);
	}
}

TEST(DisplayDecimatorTest, ActivatesOnlyWhenAggregationHasEnoughHeadroom)
{
	EXPECT_FALSE(shouldDecimateDisplaySeries(0, 1000));
	EXPECT_FALSE(shouldDecimateDisplaySeries(3000, 1000));
	EXPECT_TRUE(shouldDecimateDisplaySeries(3001, 1000));
	EXPECT_FALSE(shouldDecimateDisplaySeries(10000, 3800));
	EXPECT_TRUE(shouldDecimateDisplaySeries(10000, 1900));
}

TEST(DisplayDecimatorTest, FindsAVisibleWindowAcrossAWrappedRing)
{
	std::vector<double> x{5, 6, 7, 8, 9, 0, 1, 2, 3, 4};
	std::vector<double> y{105, 106, 107, 108, 109, 100, 101, 102, 103, 104};

	const DisplayPoints points = decimateDisplaySeries(x.data(), y.data(), 10, 5, 3.0, 7.0, 2);
	ASSERT_FALSE(points.x.empty());
	EXPECT_EQ(points.x.front(), 3.0);
	EXPECT_EQ(points.x.back(), 7.0);
	for (double value : points.x)
	{
		EXPECT_GE(value, 3.0);
		EXPECT_LE(value, 7.0);
	}
}
