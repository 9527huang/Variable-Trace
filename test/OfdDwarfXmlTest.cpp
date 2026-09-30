#include <gtest/gtest.h>

#include <string>

#include "OfdDwarfXml.hpp"

/*
 * Reading the XML dump of a TI object file.
 *
 * The reader is deliberately not a general XML implementation: the document is
 * machine generated and tens of megabytes long, so it is walked once and only
 * what the viewer uses is kept. What these tests pin down is which places a
 * value may come from, what happens to an offset that could be read two ways,
 * and which structural mistakes are refused rather than absorbed - because a
 * dump read wrongly does not fail, it produces variables at the wrong addresses.
 */

namespace
{
	const std::string source = "dump.xml";

	ofd::DwarfIndex loadDocument(const std::string& document)
	{
		ofd::DwarfIndex index;
		std::string error;

		EXPECT_TRUE(index.load(document, source, nullptr, error)) << error;

		return index;
	}

	std::string loadReason(const std::string& document)
	{
		ofd::DwarfIndex index;
		std::string error;

		EXPECT_FALSE(index.load(document, source, nullptr, error));

		return error;
	}
}  // namespace

TEST(OfdDwarfXmlTest, aDieIsFoundByItsOffset)
{
	const ofd::DwarfIndex index = loadDocument(R"(<ofd><die id="0x0000002a" tag="DW_TAG_variable">)"
											   R"(<attribute name="DW_AT_name" value="counter"/></die></ofd>)");

	ASSERT_EQ(index.size(), 1u);

	const ofd::Die* die = index.find(0x2a);

	ASSERT_NE(die, nullptr);
	EXPECT_EQ(die->offset, 0x2au);
	EXPECT_EQ(die->tag, "DW_TAG_variable");
	EXPECT_EQ(die->name, "counter");
}

TEST(OfdDwarfXmlTest, anOffsetThatIsNotInTheDocumentIsNotFound)
{
	const ofd::DwarfIndex index = loadDocument(R"(<ofd><die id="0x0000002a" tag="DW_TAG_variable"/></ofd>)");

	EXPECT_EQ(index.find(0x2b), nullptr);
	EXPECT_EQ(index.find(0), nullptr);
}

TEST(OfdDwarfXmlTest, anOffsetThatIsAlsoADecimalNumberIsFoundByBothReadings)
{
	/* Debug information is printed in hexadecimal, so `10` is sixteen. A
	   printer that meant ten would point at the wrong entry, so both readings
	   are registered; the offset the entry reports stays the hexadecimal one. */
	const ofd::DwarfIndex index = loadDocument(R"(<ofd><die id="0x00000010" tag="DW_TAG_variable"/></ofd>)");

	ASSERT_NE(index.find(0x10), nullptr);
	ASSERT_NE(index.find(10), nullptr);
	EXPECT_EQ(index.find(0x10)->offset, 0x10u);
}

TEST(OfdDwarfXmlTest, theAttributesTheViewerUsesAreRead)
{
	const ofd::DwarfIndex index = loadDocument(R"(<ofd><die id="0x00000001" tag="DW_TAG_variable">)"
											   R"(<attribute name="DW_AT_name" value="state"/>)"
											   R"(<attribute name="DW_AT_TI_symbol_name" value="_state"/>)"
											   R"(<attribute name="DW_AT_location" value="DW_OP_addr 0x8000"/>)"
											   R"(<attribute name="DW_AT_type" ref="0x00000005"/>)"
											   R"(<attribute name="DW_AT_declaration" value="1"/>)"
											   R"(<attribute name="DW_AT_external" value="true"/>)"
											   R"(</die></ofd>)");

	const ofd::Die* die = index.find(0x01);

	ASSERT_NE(die, nullptr);
	EXPECT_EQ(die->name, "state");
	EXPECT_EQ(die->symbolName, "_state");
	EXPECT_EQ(die->location, "DW_OP_addr 0x8000");
	EXPECT_TRUE(die->hasType);
	EXPECT_EQ(die->typeOffset, 5u);
	EXPECT_TRUE(die->isDeclaration);
	EXPECT_TRUE(die->isExternal);
}

TEST(OfdDwarfXmlTest, theNumbersTheWalkNeedsAreRead)
{
	const ofd::DwarfIndex index = loadDocument(R"(<ofd><die id="0x00000001" tag="DW_TAG_member">)"
											   R"(<attribute name="DW_AT_byte_size" value="2"/>)"
											   R"(<attribute name="DW_AT_encoding" value="7"/>)"
											   R"(<attribute name="DW_AT_data_member_location" value="2"/>)"
											   R"(<attribute name="DW_AT_upper_bound" value="15"/>)"
											   R"(<attribute name="DW_AT_count" value="16"/>)"
											   R"(</die></ofd>)");

	const ofd::Die* die = index.find(0x01);

	ASSERT_NE(die, nullptr);
	EXPECT_TRUE(die->hasByteSize);
	EXPECT_EQ(die->byteSize, 2u);
	EXPECT_TRUE(die->hasEncoding);
	EXPECT_EQ(die->encoding, 7u);
	EXPECT_TRUE(die->hasDataMemberOffset);
	EXPECT_EQ(die->dataMemberOffset, 2u);
	EXPECT_TRUE(die->hasUpperBound);
	EXPECT_EQ(die->upperBound, 15u);
	EXPECT_TRUE(die->hasCount);
	EXPECT_EQ(die->count, 16u);
}

TEST(OfdDwarfXmlTest, aValueIsReadFromAChildElement)
{
	/* The document puts an attribute value in one of several places depending
	   on the form of the attribute, and the forms are not published. */
	const ofd::DwarfIndex index = loadDocument(R"(<ofd><die id="0x00000001" tag="DW_TAG_base_type">)"
											   R"(<attribute name="DW_AT_byte_size"><value>2</value></attribute>)"
											   R"(</die></ofd>)");

	const ofd::Die* die = index.find(0x01);

	ASSERT_NE(die, nullptr);
	EXPECT_TRUE(die->hasByteSize);
	EXPECT_EQ(die->byteSize, 2u);
}

TEST(OfdDwarfXmlTest, characterReferencesAreDecoded)
{
	const ofd::DwarfIndex index = loadDocument(R"(<ofd><die id="0x00000001" tag="DW_TAG_variable">)"
											   R"(<attribute name="DW_AT_name" value="a&amp;b&lt;c&#48;d"/>)"
											   R"(</die></ofd>)");

	const ofd::Die* die = index.find(0x01);

	ASSERT_NE(die, nullptr);
	EXPECT_EQ(die->name, "a&b<c0d");
}

TEST(OfdDwarfXmlTest, aCommentInsideAnEntryIsSkipped)
{
	const ofd::DwarfIndex index = loadDocument(R"(<ofd><die id="0x00000001" tag="DW_TAG_variable">)"
											   R"(<!-- written by the tool -->)"
											   R"(<attribute name="DW_AT_name" value="x"/></die></ofd>)");

	ASSERT_EQ(index.size(), 1u);
	ASSERT_NE(index.find(0x01), nullptr);
	EXPECT_EQ(index.find(0x01)->name, "x");
}

TEST(OfdDwarfXmlTest, theElementNamesAreReadRegardlessOfCase)
{
	/* The tool is not consistent about the case of the element names; the
	   attribute names are, and are read as written. */
	const ofd::DwarfIndex index = loadDocument(R"(<OFD><Die id="0x00000001" tag="DW_TAG_variable"/></OFD>)");

	ASSERT_EQ(index.size(), 1u);
	EXPECT_EQ(index.find(0x01)->tag, "DW_TAG_variable");
}

TEST(OfdDwarfXmlTest, entriesInsideAWrapperAreFound)
{
	/* The tool groups entries under containers that are neither an entry nor
	   an attribute. The entries in one are siblings. */
	const ofd::DwarfIndex index = loadDocument(R"(<ofd><compilation_unit>)"
											   R"(<die id="0x00000001" tag="DW_TAG_variable"/>)"
											   R"(<die id="0x00000002" tag="DW_TAG_variable"/>)"
											   R"(</compilation_unit></ofd>)");

	ASSERT_EQ(index.size(), 2u);
	ASSERT_NE(index.find(0x01), nullptr);
	ASSERT_NE(index.find(0x02), nullptr);
	EXPECT_TRUE(index.find(0x01)->children.empty());
	EXPECT_TRUE(index.find(0x02)->children.empty());
}

TEST(OfdDwarfXmlTest, anEntryInsideAnEntryIsRecordedAsAChild)
{
	const ofd::DwarfIndex index = loadDocument(R"(<ofd><die id="0x00000001" tag="DW_TAG_structure_type">)"
											   R"(<die id="0x00000002" tag="DW_TAG_member">)"
											   R"(<attribute name="DW_AT_name" value="x"/></die>)"
											   R"(</die></ofd>)");

	ASSERT_EQ(index.size(), 2u);
	ASSERT_NE(index.find(0x01), nullptr);
	ASSERT_EQ(index.find(0x01)->children.size(), 1u);
	EXPECT_EQ(index.find(0x01)->children.front(), 0x02u);
	EXPECT_EQ(index.find(0x02)->name, "x");
	EXPECT_TRUE(index.find(0x02)->children.empty());
}

TEST(OfdDwarfXmlTest, anEntryBehindAWrapperInsideAnEntryLosesItsParent)
{
	/* A wrapper inside an entry is walked as a container, so the entries in it
	   are siblings of everything else rather than members of the entry they sit
	   in. A structure written that way would report no fields, so the walk that
	   reads members has to tolerate an empty list instead of assuming one. */
	const ofd::DwarfIndex index = loadDocument(R"(<ofd><die id="0x00000001" tag="DW_TAG_structure_type">)"
											   R"(<children_group><die id="0x00000002" tag="DW_TAG_member"/>)"
											   R"(</children_group></die></ofd>)");

	ASSERT_EQ(index.size(), 2u);
	ASSERT_NE(index.find(0x02), nullptr);
	EXPECT_TRUE(index.find(0x01)->children.empty());
}

TEST(OfdDwarfXmlTest, anEmptyDocumentIsNotAnError)
{
	/* A file can carry no debugging information at all, which is a different
	   thing from a file that cannot be read. */
	const ofd::DwarfIndex index = loadDocument(R"(<?xml version="1.0" encoding="UTF-8"?><ofd/>)");

	EXPECT_EQ(index.size(), 0u);
	EXPECT_EQ(index.find(0), nullptr);
}

TEST(OfdDwarfXmlTest, aDocumentWithNoEntryInItIsNotAnError)
{
	const ofd::DwarfIndex index = loadDocument(R"(<ofd><compilation_unit name="main.c"/></ofd>)");

	EXPECT_EQ(index.size(), 0u);
}

TEST(OfdDwarfXmlTest, textThatIsNotADocumentIsRefused)
{
	EXPECT_EQ(loadReason("this is not a document"), "no element found");
}

TEST(OfdDwarfXmlTest, anUnterminatedEntryIsRefused)
{
	EXPECT_EQ(loadReason("<ofd><die id=\"0x00000001\" tag=\"DW_TAG_variable\"/>"), "unterminated element 'ofd'");
}

TEST(OfdDwarfXmlTest, anEntryClosedByAnotherNameIsRefused)
{
	EXPECT_EQ(loadReason("<ofd><a></b></ofd>"), "element 'a' closed by 'b'");
}

TEST(OfdDwarfXmlTest, anAttributeWithoutAValueIsRefused)
{
	EXPECT_EQ(loadReason(R"(<ofd><die id/></ofd>)"), "attribute 'id' has no value");
}

TEST(OfdDwarfXmlTest, anAttributeWithAnUnquotedValueIsRefused)
{
	EXPECT_EQ(loadReason(R"(<ofd><die id=5/></ofd>)"), "attribute 'id' is not quoted");
}

TEST(OfdDwarfXmlTest, anUnterminatedAttributeValueIsRefused)
{
	EXPECT_EQ(loadReason(R"(<ofd><die id="5/></ofd>)"), "unterminated value of attribute 'id'");
}

TEST(OfdDwarfXmlTest, theAggregateTagsAreTheOnesWhoseMembersAreReportedOneByOne)
{
	/* An enumeration is not one of them: its members name values, they are not
	   fields to read. */
	EXPECT_TRUE(ofd::isAggregateTag("DW_TAG_structure_type"));
	EXPECT_TRUE(ofd::isAggregateTag("DW_TAG_union_type"));
	EXPECT_TRUE(ofd::isAggregateTag("DW_TAG_class_type"));

	EXPECT_FALSE(ofd::isAggregateTag("DW_TAG_enumeration_type"));
	EXPECT_FALSE(ofd::isAggregateTag("DW_TAG_base_type"));
	EXPECT_FALSE(ofd::isAggregateTag(""));
}

TEST(OfdDwarfXmlTest, theTransparentTagsAreTheOnesThatHideTheRealType)
{
	EXPECT_TRUE(ofd::isTransparentTag("DW_TAG_typedef"));
	EXPECT_TRUE(ofd::isTransparentTag("DW_TAG_const_type"));
	EXPECT_TRUE(ofd::isTransparentTag("DW_TAG_volatile_type"));
	EXPECT_TRUE(ofd::isTransparentTag("DW_TAG_TI_far_type"));
	EXPECT_TRUE(ofd::isTransparentTag("DW_TAG_restrict_type"));

	EXPECT_FALSE(ofd::isTransparentTag("DW_TAG_structure_type"));
	EXPECT_FALSE(ofd::isTransparentTag(""));
}
