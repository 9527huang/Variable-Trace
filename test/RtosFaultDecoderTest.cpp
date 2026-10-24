#include <gtest/gtest.h>

#include "RtosFaultDecoder.hpp"

TEST(RtosFaultDecoderTest, ANullFunctionPointerIsNamedAsSuch)
{
	/* INVSTATE with an even PC: the branch went somewhere that is not Thumb
	   code. This is what `((void (*)(void))0)();` does, and it is the crash
	   people meet most often without being told what it looks like. */
	const RtosFaultReport report = decodeFault(0x00020000u, 0x00000010u, 0xFFFFFFF9u);

	ASSERT_FALSE(report.empty());
	EXPECT_EQ("INVSTATE", report.lines.front().code);
	EXPECT_NE(std::string::npos, report.lines.front().meaning.find("function pointer"));
}

TEST(RtosFaultDecoderTest, APreciseBusFaultKeepsTheWriteFlag)
{
	const RtosFaultReport report = decodeFault(0x00000200u, 0x08001A2Cu, 0x08001A01u);

	ASSERT_EQ(1u, report.lines.size());
	EXPECT_EQ("PRECISERR", report.lines.front().code);
	EXPECT_EQ(0x08001A2Cu, report.pc);

	/* With BFARVALID set as well, the target is saying it captured the
	   address - which this build does not carry, and says it does not. */
	const RtosFaultReport captured = decodeFault(0x00008200u, 0x08001A2Cu, 0);

	ASSERT_EQ(2u, captured.lines.size());
	EXPECT_EQ("PRECISERR", captured.lines[0].code);
	EXPECT_EQ("BFARVALID", captured.lines[1].code);
}

TEST(RtosFaultDecoderTest, AnImpreciseFaultSaysThePcIsNotTheCause)
{
	/* The store buffer reported it one instruction late; a report that pointed
	   at this PC without saying so would send the reader to the wrong line. */
	const RtosFaultReport report = decodeFault(0x00000400u, 0x08001A2Cu, 0);

	ASSERT_EQ(1u, report.lines.size());
	EXPECT_EQ("IMPRECISERR", report.lines.front().code);
	EXPECT_NE(std::string::npos, report.lines.front().meaning.find("PC points past"));
}

TEST(RtosFaultDecoderTest, SeveralFindingsComeOutInRegisterOrder)
{
	/* Bus half first, then the usage half: the register is read in the order it
	   is laid out, so the report never has to be re-sorted by eye. */
	const RtosFaultReport report = decodeFault(0x00000200u | 0x00030000u, 0x1000, 0);

	ASSERT_EQ(3u, report.lines.size());
	EXPECT_EQ("PRECISERR", report.lines[0].code);
	EXPECT_EQ("UNDEFINSTR", report.lines[1].code);
	EXPECT_EQ("INVSTATE", report.lines[2].code);
	EXPECT_NE(std::string::npos, report.summary().find("precise"));
}

TEST(RtosFaultDecoderTest, AnEmptyStatusRegisterSaysSoRatherThanClaimingNothingFailed)
{
	const RtosFaultReport report = decodeFault(0, 0x08001234u, 0);

	EXPECT_TRUE(report.empty());
	EXPECT_NE(std::string::npos, report.summary().find("status register was empty"));
}

TEST(RtosFaultDecoderTest, AnUnnamedBitIsReportedWithTheRegisterValue)
{
	/* Bit 5 of the usage half is not one this build names. Saying nothing would
	   turn a real fault into "no findings". */
	const RtosFaultReport report = decodeFault(0x00200000u, 0x1000, 0);

	ASSERT_EQ(1u, report.lines.size());
	EXPECT_EQ("CFSR", report.lines.front().code);
	EXPECT_NE(std::string::npos, report.lines.front().meaning.find("0x00200000"));
}
