#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <numeric>

#include "ScrollingBuffer.hpp"

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