#include <gtest/gtest.h>

#include <array>
#include <iostream>

#include "Variable.hpp"

TEST(VariableTest, testSignedFracPositive)
{
	Variable var{"test"};

	var.setType(Variable::Type::I16);
	var.setHighLevelType(Variable::HighLevelType::SIGNEDFRAC);
	var.setFractional({15, 1.0});
	var.setRawValue(32767);
	double value = var.transformToDouble();

	ASSERT_NEAR(value, 1.0, 10e-3);
}

TEST(VariableTest, testSignedFracNegative)
{
	Variable var{"test"};

	var.setType(Variable::Type::I16);
	var.setHighLevelType(Variable::HighLevelType::SIGNEDFRAC);
	var.setFractional({15, 1.0});
	var.setRawValue(-32768);
	double value = var.transformToDouble();

	ASSERT_NEAR(value, -1.0, 10e-6);
}

TEST(VariableTest, testSignedFracUnsignedVarPositive)
{
	Variable var{"test"};

	var.setType(Variable::Type::U16);
	var.setHighLevelType(Variable::HighLevelType::SIGNEDFRAC);
	var.setFractional({15, 1.0});
	var.setRawValue(32767);
	double value = var.transformToDouble();

	ASSERT_NEAR(value, 1.0, 10e-3);
}

TEST(VariableTest, testSignedFracUnsignedVarNegative)
{
	Variable var{"test"};

	var.setType(Variable::Type::U16);
	var.setHighLevelType(Variable::HighLevelType::SIGNEDFRAC);
	var.setFractional({15, 1.0});
	var.setRawValue(-32768);
	double value = var.transformToDouble();

	ASSERT_NEAR(value, -1.0, 10e-6);
}

/** Unsigned Fractional ***/

TEST(VariableTest, testUnsignedFracPositive)
{
	Variable var{"test"};

	var.setType(Variable::Type::I16);
	var.setHighLevelType(Variable::HighLevelType::UNSIGNEDFRAC);
	var.setFractional({15, 1.0});
	var.setRawValue(32767);
	double value = var.transformToDouble();

	ASSERT_NEAR(value, 1.0, 10e-3);
}

TEST(VariableTest, testUnsignedFracNegative)
{
	Variable var{"test"};

	var.setType(Variable::Type::I16);
	var.setHighLevelType(Variable::HighLevelType::UNSIGNEDFRAC);
	var.setFractional({15, 1.0});
	var.setRawValue(65534);
	double value = var.transformToDouble();

	ASSERT_NEAR(value, 2.0, 10e-3);
}

TEST(VariableTest, testUnsignedFracUnsignedVarPositive)
{
	Variable var{"test"};

	var.setType(Variable::Type::U16);
	var.setHighLevelType(Variable::HighLevelType::UNSIGNEDFRAC);
	var.setFractional({16, 1.0});
	var.setRawValue(32767);
	double value = var.transformToDouble();

	ASSERT_NEAR(value, 0.5, 10e-3);
}

TEST(VariableTest, testUnsignedFracUnsignedVarNegative)
{
	Variable var{"test"};

	var.setType(Variable::Type::U16);
	var.setHighLevelType(Variable::HighLevelType::UNSIGNEDFRAC);
	var.setFractional({16, 1.0});
	var.setRawValue(65534);
	double value = var.transformToDouble();

	ASSERT_NEAR(value, 1.0, 10e-3);
}

/* ------------------------------------------------------------ write limits */

TEST(VariableTest, testWriteIsAllowedUntilLimitsAreConfigured)
{
	Variable var{"test"};

	/* Off by default, so an existing project keeps writing whatever it did
	   before the guard existed. */
	ASSERT_FALSE(var.getWriteLimitsEnabled());
	ASSERT_TRUE(var.isWriteAllowed(-12345.0));
	ASSERT_TRUE(var.isWriteAllowed(1e9));
}

TEST(VariableTest, testWriteLimitsAreInclusive)
{
	Variable var{"test"};

	var.setWriteLimits(true, 10.0, 20.0);

	ASSERT_TRUE(var.getWriteLimitsEnabled());
	ASSERT_DOUBLE_EQ(var.getWriteMin(), 10.0);
	ASSERT_DOUBLE_EQ(var.getWriteMax(), 20.0);

	ASSERT_TRUE(var.isWriteAllowed(10.0));
	ASSERT_TRUE(var.isWriteAllowed(15.0));
	ASSERT_TRUE(var.isWriteAllowed(20.0));
	ASSERT_FALSE(var.isWriteAllowed(9.99));
	ASSERT_FALSE(var.isWriteAllowed(20.01));
}

TEST(VariableTest, testWriteLimitsCanBeTurnedOffWithoutLosingTheRange)
{
	Variable var{"test"};

	var.setWriteLimits(true, 0.0, 100.0);
	var.setWriteLimits(false, 0.0, 100.0);

	/* The numbers stay, so a checkbox that is ticked again brings the same
	   range back rather than an empty one. */
	ASSERT_FALSE(var.getWriteLimitsEnabled());
	ASSERT_DOUBLE_EQ(var.getWriteMin(), 0.0);
	ASSERT_DOUBLE_EQ(var.getWriteMax(), 100.0);
	ASSERT_TRUE(var.isWriteAllowed(1000.0));
}

/* ------------------------------------------------------------------- enum */

TEST(VariableTest, testEnumIsOffByDefault)
{
	Variable var{"test"};

	ASSERT_FALSE(var.isEnum());
	ASSERT_TRUE(var.getEnumLabels().empty());
	ASSERT_EQ(var.getEnumLabel(3.0), "");
}

TEST(VariableTest, testBothEnumInterpretationsCountAsEnum)
{
	Variable var{"test"};

	var.setHighLevelType(Variable::HighLevelType::NONE);
	ASSERT_FALSE(var.isEnum());

	var.setHighLevelType(Variable::HighLevelType::SIGNEDFRAC);
	ASSERT_FALSE(var.isEnum());

	var.setHighLevelType(Variable::HighLevelType::ENUM);
	ASSERT_TRUE(var.isEnum());

	var.setHighLevelType(Variable::HighLevelType::CUSTOM_ENUM);
	ASSERT_TRUE(var.isEnum());
}

TEST(VariableTest, testEnumLabelIsLookedUpByValue)
{
	Variable var{"state"};

	var.setHighLevelType(Variable::HighLevelType::ENUM);
	var.setEnumLabels({{"idle", 0}, {"running", 1}, {"fault", 7}});

	ASSERT_EQ(var.getEnumLabel(0.0), "idle");
	ASSERT_EQ(var.getEnumLabel(1.0), "running");
	ASSERT_EQ(var.getEnumLabel(7.0), "fault");

	/* A value without a name is drawn as a number, which is what the empty
	   answer tells the caller to do. */
	ASSERT_EQ(var.getEnumLabel(2.0), "");

	ASSERT_EQ(var.getEnumLabels().size(), 3u);
	ASSERT_EQ(var.getEnumLabels().front().label, "idle");
	ASSERT_EQ(var.getEnumLabels().back().value, 7);
}

TEST(VariableTest, testEnumLabelIgnoresAFraction)
{
	Variable var{"state"};

	var.setHighLevelType(Variable::HighLevelType::CUSTOM_ENUM);
	var.setEnumLabels({{"zero", 0}, {"one", 1}});

	/* The value that arrives is what postprocessing left behind, and a fixed
	   point reading of it can carry a fraction that still names the same
	   state. */
	ASSERT_EQ(var.getEnumLabel(1.4), "one");
	ASSERT_EQ(var.getEnumLabel(0.9), "zero");
	ASSERT_EQ(var.getEnumLabel(-0.5), "zero");
}

TEST(VariableTest, testEnumLabelsAreIgnoredWhenTheVariableIsNotAnEnum)
{
	Variable var{"state"};

	var.setHighLevelType(Variable::HighLevelType::ENUM);
	var.setEnumLabels({{"idle", 0}});
	ASSERT_EQ(var.getEnumLabel(0.0), "idle");

	var.setHighLevelType(Variable::HighLevelType::NONE);

	/* The list stays on the variable, but it is no longer consulted, so the
	   panel cannot show a name for a variable the user reads as a number. */
	ASSERT_EQ(var.getEnumLabel(0.0), "");
	ASSERT_EQ(var.getEnumLabels().size(), 1u);
}
