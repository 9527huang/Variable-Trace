#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "FreeRtosProfile.hpp"
#include "GdbRtosLayout.hpp"

namespace
{
	/*
	 * A real `ptype /o ListItem_t`, MI escapes and all.
	 *
	 * The escapes are kept because they are part of what the parser has to
	 * tolerate, and so is the tag name: gdb prints `xLIST_ITEM` while the
	 * profile asks about `ListItem_t`.
	 */
	const std::string listItemDump =
	    "=thread-group-added,id=\"i1\"\n"
	    "&\"warning: could not convert 'ListItem_t' from the host encoding.\\n\"\n"
	    "~\"type = struct xLIST_ITEM {\\n\"\n"
	    "~\"/*      0      |       4 */    TickType_t xItemValue;\\n\"\n"
	    "~\"/*      4      |       4 */    struct xLIST_ITEM *pxNext;\\n\"\n"
	    "~\"/*      8      |       4 */    struct xLIST_ITEM *pxPrevious;\\n\"\n"
	    "~\"/*     12      |       4 */    void *pvOwner;\\n\"\n"
	    "~\"/*     16      |       4 */    struct xLIST *pxContainer;\\n\"\n"
	    "~\"\\n\"\n"
	    "~\"                               /* total size (bytes):   20 */\\n\"\n"
	    "~\"                             }\\n\"\n"
	    "(gdb) \n";

	std::string fieldListDump()
	{
		return "~\"type = struct xLIST {\\n\"\n"
		       "~\"/*      0      |       4 */    volatile UBaseType_t uxNumberOfItems;\\n\"\n"
		       "~\"/*      4      |       4 */    struct xLIST_ITEM *pxIndex;\\n\"\n"
		       "~\"/*      8      |      12 */    MiniListItem_t xListEnd;\\n\"\n"
		       "~\"/* total size (bytes):   20 */\\n\"\n"
		       "~\"}\\n\"\n";
	}
}  // namespace

TEST(GdbRtosLayout, ReadsTheSizeAndEveryField)
{
	RtosLayoutTable table;

	ASSERT_TRUE(GdbRtosLayout::parseStructDump(listItemDump, "ListItem_t", table));

	uint32_t value = 0;

	ASSERT_TRUE(table.structSize("ListItem_t", value));
	EXPECT_EQ(value, 20u);

	ASSERT_TRUE(table.fieldOffset("ListItem_t", "xItemValue", value));
	EXPECT_EQ(value, 0u);

	ASSERT_TRUE(table.fieldOffset("ListItem_t", "pxNext", value));
	EXPECT_EQ(value, 4u);

	ASSERT_TRUE(table.fieldOffset("ListItem_t", "pvOwner", value));
	EXPECT_EQ(value, 12u);

	ASSERT_TRUE(table.fieldOffset("ListItem_t", "pxContainer", value));
	EXPECT_EQ(value, 16u);
}

TEST(GdbRtosLayout, TakesTheNameItWasAskedForRatherThanTheTag)
{
	RtosLayoutTable table;

	ASSERT_TRUE(GdbRtosLayout::parseStructDump(listItemDump, "ListItem_t", table));

	/* The dump says xLIST_ITEM. Filing the fields under that name would leave
	   every lookup by the profile's name answering nothing. */
	uint32_t value = 0;

	EXPECT_TRUE(table.fieldOffset("ListItem_t", "pxNext", value));
	EXPECT_FALSE(table.fieldOffset("xLIST_ITEM", "pxNext", value));
}

TEST(GdbRtosLayout, StripsTheStarFromAPointer)
{
	RtosLayoutTable table;

	ASSERT_TRUE(GdbRtosLayout::parseStructDump(listItemDump, "ListItem_t", table));

	uint32_t value = 0;

	/* `struct xLIST_ITEM *pxNext;` ends in `*pxNext`, and the star is the
	   declaration's, not the name's. */
	EXPECT_TRUE(table.fieldOffset("ListItem_t", "pxNext", value));
	EXPECT_FALSE(table.fieldOffset("ListItem_t", "*pxNext", value));
}

TEST(GdbRtosLayout, CutsAnArrayExtentOffTheName)
{
	RtosLayoutTable table;

	ASSERT_TRUE(GdbRtosLayout::parseStructDump(fieldListDump(), "List_t", table));

	uint32_t value = 0;

	ASSERT_TRUE(table.fieldOffset("List_t", "uxNumberOfItems", value));
	EXPECT_EQ(value, 0u);

	ASSERT_TRUE(table.fieldOffset("List_t", "pxIndex", value));
	EXPECT_EQ(value, 4u);

	ASSERT_TRUE(table.fieldOffset("List_t", "xListEnd", value));
	EXPECT_EQ(value, 8u);

	EXPECT_FALSE(table.fieldOffset("List_t", "xListEnd[1]", value));
}

TEST(GdbRtosLayout, EveryOffsetItReadsIsMarkedAsMeasured)
{
	RtosLayoutTable table;

	ASSERT_TRUE(GdbRtosLayout::parseStructDump(listItemDump, "ListItem_t", table));

	/* Nothing here was written down by hand, so the table must not present
	   itself as carrying fallbacks. */
	EXPECT_FALSE(table.isAllFallback());
	EXPECT_FALSE(table.isFallback("ListItem_t", "pxNext"));
	EXPECT_EQ(table.structOrigin("ListItem_t"), RtosLayoutTable::Origin::Measured);
}

TEST(GdbRtosLayout, SaysNothingAboutADumpThatNamesNoStructure)
{
	RtosLayoutTable table;

	EXPECT_FALSE(GdbRtosLayout::parseStructDump("No symbol table is loaded.\n(gdb) \n", "TCB_t", table));
	EXPECT_EQ(table.structCount(), 0u);
	EXPECT_EQ(table.fieldCount(), 0u);
}

TEST(GdbRtosLayout, TakesTheStructureListFromTheProfileWithoutRepeats)
{
	FreeRtosProfile profile;

	const std::vector<std::string> structures = GdbRtosLayout::structuresOf(profile);

	EXPECT_FALSE(structures.empty());

	/* The profile names many fields per structure; the list is one entry each. */
	std::vector<std::string> unique = structures;
	std::sort(unique.begin(), unique.end());

	EXPECT_EQ(std::unique(unique.begin(), unique.end()), unique.end());

	EXPECT_NE(std::find(structures.begin(), structures.end(), "TCB_t"), structures.end());
	EXPECT_NE(std::find(structures.begin(), structures.end(), "List_t"), structures.end());
	EXPECT_NE(std::find(structures.begin(), structures.end(), "ListItem_t"), structures.end());
}
