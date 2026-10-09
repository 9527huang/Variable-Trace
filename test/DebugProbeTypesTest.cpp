#include <gtest/gtest.h>

#include <cstdint>

#include "IDebugProbe.hpp"

/*
 * The names the interface puts on a probe type.
 *
 * The acquisition window's list and the connection indicator both index the
 * table by the value of the enum, so the two only agree while the table is in
 * the order of the enum. They used to keep a list each, and the one the
 * indicator read was three entries long while the enum had four: a GDB server
 * was offered in the window and then reported as STLINK up in the menu bar.
 *
 * The number of entries is held down by a static_assert in the header, which
 * catches a probe added without a name. It cannot catch a table that was
 * reordered, because that leaves the count alone and simply hands every probe
 * the name of another one - which is what the first test here is for.
 */

TEST(DebugProbeTypesTest, everyProbeIsNamedInTheOrderOfTheEnum)
{
	ASSERT_EQ(IDebugProbe::probeNameCount, static_cast<int>(IDebugProbe::Probe::Count));

	EXPECT_STREQ(IDebugProbe::probeNames[IDebugProbe::Probe::Stlink], "STLINK");
	EXPECT_STREQ(IDebugProbe::probeNames[IDebugProbe::Probe::Jlink], "JLINK");
	EXPECT_STREQ(IDebugProbe::probeNames[IDebugProbe::Probe::Serial], "SERIAL");
	EXPECT_STREQ(IDebugProbe::probeNames[IDebugProbe::Probe::GdbServer], "GDBSERVER");
}

TEST(DebugProbeTypesTest, theValuesInAProjectFileKeepTheirMeaning)
{
	/* The value is written into a project file, so a project saved before the
	   GDB server existed has to keep opening with the probe it was saved with. */
	EXPECT_EQ(static_cast<uint32_t>(IDebugProbe::Probe::Stlink), 0u);
	EXPECT_EQ(static_cast<uint32_t>(IDebugProbe::Probe::Jlink), 1u);
	EXPECT_EQ(static_cast<uint32_t>(IDebugProbe::Probe::Serial), 2u);
	EXPECT_EQ(static_cast<uint32_t>(IDebugProbe::Probe::GdbServer), 3u);
}
