#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "SerialDriverLayout.hpp"

/*
 * Reading the settings the target side serial driver publishes.
 *
 * What the host gets wrong here is not a crash but a silent limit: the number
 * of variables a single read may carry is the driver's, and a host that assumes
 * a larger one gets nothing back, because the driver drops an oversized frame
 * instead of truncating it. The struct is small enough that the whole of the
 * reading is the offsets, which is what these tests pin down.
 */

namespace
{
	/* The struct as the target lays it out: three 32 bit numbers, each least
	   significant byte first. */
	std::vector<uint8_t> settingsBytes(uint32_t version, uint32_t revision, uint32_t maxVariables)
	{
		const uint32_t values[3] = {version, revision, maxVariables};

		std::vector<uint8_t> bytes(serial::driverSettingsSize, 0);

		for (size_t field = 0; field < 3; field++)
		{
			for (size_t byte = 0; byte < 4; byte++)
				bytes[field * 4 + byte] = static_cast<uint8_t>((values[field] >> (8 * byte)) & 0xFF);
		}

		return bytes;
	}
}  // namespace

TEST(SerialDriverLayoutTest, theThreeFieldsAreReadInOrder)
{
	const std::vector<uint8_t> bytes = settingsBytes(1, 2, 32);

	const serial::DriverSettings settings = serial::parseDriverSettings(bytes.data(), bytes.size());

	EXPECT_EQ(settings.version, 1u);
	EXPECT_EQ(settings.revision, 2u);
	EXPECT_EQ(settings.maxVariables, 32u);
}

TEST(SerialDriverLayoutTest, aNumberOfTheFullWidthIsReadCorrectly)
{
	/* A byte order mistake shows up on the bytes above eight, which is where
	   worth is done in the lower ones. */
	const std::vector<uint8_t> bytes = settingsBytes(1, 2, 0x01020304u);

	const serial::DriverSettings settings = serial::parseDriverSettings(bytes.data(), bytes.size());

	EXPECT_EQ(settings.maxVariables, 0x01020304u);
}

TEST(SerialDriverLayoutTest, aBufferThatIsTooShortYieldsNothing)
{
	/* One byte of the last field missing. Reading it anyway would produce a
	   limit that is wrong rather than absent. */
	const std::vector<uint8_t> bytes = settingsBytes(1, 2, 32);

	const serial::DriverSettings settings = serial::parseDriverSettings(bytes.data(), serial::driverSettingsSize - 1);

	EXPECT_EQ(settings.version, 0u);
	EXPECT_EQ(settings.revision, 0u);
	EXPECT_EQ(settings.maxVariables, 0u);
}

TEST(SerialDriverLayoutTest, noBufferAtAllYieldsNothing)
{
	const serial::DriverSettings settings = serial::parseDriverSettings(nullptr, 0);

	EXPECT_EQ(settings.version, 0u);
	EXPECT_FALSE(serial::isSupported(settings));
}

TEST(SerialDriverLayoutTest, bytesPastTheStructAreIgnored)
{
	/* The read that fetched the struct may have been longer than it. */
	std::vector<uint8_t> bytes = settingsBytes(1, 2, 16);
	bytes.resize(64, 0xFF);

	const serial::DriverSettings settings = serial::parseDriverSettings(bytes.data(), bytes.size());

	EXPECT_EQ(settings.maxVariables, 16u);
}

TEST(SerialDriverLayoutTest, theVersionThisBuildKnowsIsSupported)
{
	EXPECT_TRUE(serial::isSupported({serial::supportedVersion, serial::supportedRevision, 32}));
}

TEST(SerialDriverLayoutTest, anOlderRevisionOfTheSameVersionIsSupported)
{
	EXPECT_TRUE(serial::isSupported({serial::supportedVersion, 0, 32}));
}

TEST(SerialDriverLayoutTest, aNewerRevisionIsNot)
{
	/* A later revision may have added a field, and reading a struct at the
	   wrong offsets produces numbers rather than an error. */
	EXPECT_FALSE(serial::isSupported({serial::supportedVersion, serial::supportedRevision + 1, 32}));
}

TEST(SerialDriverLayoutTest, aDifferentVersionIsNot)
{
	EXPECT_FALSE(serial::isSupported({serial::supportedVersion + 1, 0, 32}));
}

TEST(SerialDriverLayoutTest, aZeroedStructIsNotADriver)
{
	EXPECT_FALSE(serial::isSupported({0, 0, 0}));
	EXPECT_EQ(serial::describeDriverSettings({0, 0, 0}), "Serial driver not detected.");
}

TEST(SerialDriverLayoutTest, theDescriptionNamesTheRevisionAndTheLimit)
{
	EXPECT_EQ(serial::describeDriverSettings({1, 2, 32}), "Serial driver 1.2, up to 32 variables per read");
}

TEST(SerialDriverLayoutTest, theDescriptionLeavesOutALimitItWasNotTold)
{
	/* Zero is what an older driver that has no such field reports. */
	EXPECT_EQ(serial::describeDriverSettings({1, 0, 0}), "Serial driver 1.0");
}

TEST(SerialDriverLayoutTest, theDescriptionSaysWhenTheRevisionIsNotThisOnes)
{
	const std::string text = serial::describeDriverSettings({1, 3, 10});

	EXPECT_EQ(text, "Serial driver 1.3, up to 10 variables per read (a version this build was not written against)");
}
