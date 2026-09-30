#include <gtest/gtest.h>

#include <vector>

#include "IElfParser.hpp"
#include "LibDwarfParser.hpp"

/*
 * The two pieces of reading a DWARF file that are arithmetic rather than
 * parsing: what a location expression says, and what an encoding and a size
 * mean. They are tested here because they are the only part of the reader that
 * can be checked without a file, and because getting them wrong is silent: an
 * address that is off by one byte reads a neighbouring variable, and a sign
 * that is lost turns a small negative number into a large positive one.
 */

namespace
{
	constexpr uint8_t opAddr = 0x03;
	constexpr uint8_t opAddrx = 0xa1;

	/* The encodings the standard assigns, written out so that the test states
	   what it expects rather than repeating a header. */
	constexpr uint64_t encodingBoolean = 0x02;
	constexpr uint64_t encodingFloat = 0x04;
	constexpr uint64_t encodingSigned = 0x05;
	constexpr uint64_t encodingSignedChar = 0x06;
	constexpr uint64_t encodingUnsigned = 0x07;
	constexpr uint64_t encodingUnsignedChar = 0x08;
	constexpr uint64_t encodingAddress = 0x01;
}  // namespace

TEST(DwarfDecodeTest, addressOperationCarriesTheAddress)
{
	/* DW_OP_addr 0x20000080 on a target whose addresses are four bytes wide. */
	const std::vector<uint8_t> expression{opAddr, 0x80, 0x00, 0x00, 0x20};

	const LibDwarfParser::Location location = LibDwarfParser::decodeLocation(expression, 4);

	EXPECT_EQ(location.form, LibDwarfParser::LocationForm::Address);
	EXPECT_EQ(location.value, 0x20000080u);
}

TEST(DwarfDecodeTest, anAddressIsReadLeastSignificantByteFirst)
{
	/* The operand is a target address and is stated in the target's byte order.
	   Every target this reader is used with is little-endian, so the order is
	   written into the function rather than asked for, and a byte reversed
	   operand would produce a valid address holding the wrong value. Pinning
	   the order here is what makes such a change show up as a failure. */
	const std::vector<uint8_t> expression{opAddr, 0x20, 0x00, 0x00, 0x80};

	const LibDwarfParser::Location location = LibDwarfParser::decodeLocation(expression, 4);

	ASSERT_EQ(location.form, LibDwarfParser::LocationForm::Address);
	EXPECT_EQ(location.value, 0x80000020u);
}

TEST(DwarfDecodeTest, addressOperationWithATruncatedOperandYieldsNothing)
{
	/* Four bytes of operand promised, three present. Guessing the missing one
	   would produce an address that is wrong rather than missing. */
	const std::vector<uint8_t> expression{opAddr, 0x80, 0x00, 0x00};

	const LibDwarfParser::Location location = LibDwarfParser::decodeLocation(expression, 4);

	EXPECT_EQ(location.form, LibDwarfParser::LocationForm::Empty);
}

TEST(DwarfDecodeTest, indexedAddressIsNotAnAddress)
{
	/* The operand is an index into a section this reader does not open, so the
	   value it holds is not an address and must not be reported as one. */
	const std::vector<uint8_t> expression{opAddrx, 0x05};

	const LibDwarfParser::Location location = LibDwarfParser::decodeLocation(expression, 4);

	EXPECT_EQ(location.form, LibDwarfParser::LocationForm::AddressIndex);
	EXPECT_EQ(location.value, 5u);
}

TEST(DwarfDecodeTest, indexedAddressWithAnUnfinishedIndexYieldsNothing)
{
	/* A ULEB128 whose last byte has the continuation bit set is not a number. */
	const std::vector<uint8_t> expression{opAddrx, 0x85, 0x80};

	const LibDwarfParser::Location location = LibDwarfParser::decodeLocation(expression, 4);

	EXPECT_EQ(location.form, LibDwarfParser::LocationForm::Empty);
}

TEST(DwarfDecodeTest, indexedAddressWithMoreBytesThanAValueHoldsYieldsNothing)
{
	/* Every byte promises another one, past the width of the value the index is
	   read into. Shifting by more than the width is not a number either. */
	const std::vector<uint8_t> expression{opAddrx, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
										  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

	const LibDwarfParser::Location location = LibDwarfParser::decodeLocation(expression, 4);

	EXPECT_EQ(location.form, LibDwarfParser::LocationForm::Empty);
}

TEST(DwarfDecodeTest, anIndexThatFillsTheValueIsAccepted)
{
	/* Ten bytes is the most a ULEB128 of this width can be, and the last of
	   them ends it. */
	const std::vector<uint8_t> expression{opAddrx, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
										  0xFF, 0xFF, 0xFF, 0xFF, 0x01};

	const LibDwarfParser::Location location = LibDwarfParser::decodeLocation(expression, 4);

	ASSERT_EQ(location.form, LibDwarfParser::LocationForm::AddressIndex);
	EXPECT_EQ(location.value, 0xFFFFFFFFFFFFFFFFull);
}

TEST(DwarfDecodeTest, aRegisterLocationHasNoFixedAddress)
{
	/* DW_OP_reg5, which is where a value lives while the program runs. */
	const std::vector<uint8_t> expression{0x55};

	const LibDwarfParser::Location location = LibDwarfParser::decodeLocation(expression, 4);

	EXPECT_EQ(location.form, LibDwarfParser::LocationForm::Unsupported);
}

TEST(DwarfDecodeTest, anEmptyExpressionHasNothingInIt)
{
	const LibDwarfParser::Location location = LibDwarfParser::decodeLocation({}, 4);

	EXPECT_EQ(location.form, LibDwarfParser::LocationForm::Empty);
}

TEST(DwarfDecodeTest, unsignedEncodingAndSizePickTheType)
{
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingUnsigned, 1), Variable::Type::U8);
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingUnsigned, 2), Variable::Type::U16);
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingUnsigned, 4), Variable::Type::U32);
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingUnsignedChar, 1), Variable::Type::U8);
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingBoolean, 1), Variable::Type::U8);
}

TEST(DwarfDecodeTest, signedEncodingAndSizePickTheType)
{
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingSigned, 1), Variable::Type::I8);
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingSigned, 2), Variable::Type::I16);
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingSigned, 4), Variable::Type::I32);
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingSignedChar, 1), Variable::Type::I8);
}

TEST(DwarfDecodeTest, onlyAFourByteFloatCanBeRead)
{
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingFloat, 4), Variable::Type::F32);

	/* A double and a half precision float have no representation in the viewer,
	   and reporting the wrong width would misread every value. */
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingFloat, 8), Variable::Type::UNKNOWN);
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingFloat, 2), Variable::Type::UNKNOWN);
}

TEST(DwarfDecodeTest, anUnsupportedWidthIsUnknown)
{
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingUnsigned, 8), Variable::Type::UNKNOWN);
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingSigned, 3), Variable::Type::UNKNOWN);
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingUnsigned, 0), Variable::Type::UNKNOWN);
}

TEST(DwarfDecodeTest, anEncodingThatIsNotANumberIsUnknown)
{
	/* An address, a pointer and a complex number are all things the viewer has
	   no way to draw. */
	EXPECT_EQ(IElfParser::typeFromEncoding(encodingAddress, 4), Variable::Type::UNKNOWN);
	EXPECT_EQ(IElfParser::typeFromEncoding(0x02 + 0x10, 4), Variable::Type::UNKNOWN);
	EXPECT_EQ(IElfParser::typeFromEncoding(0x00, 4), Variable::Type::UNKNOWN);
}

TEST(DwarfDecodeTest, theTwoReadersAgreeAboutAnEncoding)
{
	/* The C2000 reader asks the shared mapping, so the two cannot drift apart. */
	EXPECT_EQ(LibDwarfParser::typeFromEncoding(encodingSigned, 2), IElfParser::typeFromEncoding(encodingSigned, 2));
	EXPECT_EQ(LibDwarfParser::typeFromEncoding(encodingFloat, 4), IElfParser::typeFromEncoding(encodingFloat, 4));
	EXPECT_EQ(LibDwarfParser::typeFromEncoding(encodingUnsigned, 8), IElfParser::typeFromEncoding(encodingUnsigned, 8));
}
