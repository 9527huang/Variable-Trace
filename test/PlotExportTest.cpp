#include <gtest/gtest.h>

#include <string>

#include "PlotExport.hpp"

/*
 * The rules behind the file names of an export. They decide what lands in the
 * directory, so they are worth stating on their own rather than being read out
 * of the code that presses the shutter.
 */

TEST(PlotExportTest, theDefaultNameKeepsItsShape)
{
	plotExport::Settings settings;

	EXPECT_EQ(plotExport::imageFileName(settings, 0, 1), "MCUViewer_screenshot.png");
}

TEST(PlotExportTest, anEmptyNameFallsBackToTheBuiltInOne)
{
	plotExport::Settings settings;
	settings.fileName = "";

	EXPECT_EQ(plotExport::imageFileName(settings, 0, 1), std::string(plotExport::defaultFileName) + ".png");
}

TEST(PlotExportTest, theExtensionIsAddedOnlyWhenItIsMissing)
{
	EXPECT_EQ(plotExport::withPngExtension("shot"), "shot.png");
	EXPECT_EQ(plotExport::withPngExtension("shot.PNG"), "shot.PNG");
	EXPECT_EQ(plotExport::withPngExtension("shot.png"), "shot.png");
	EXPECT_EQ(plotExport::withPngExtension("shot.bmp"), "shot.bmp.png");
}

TEST(PlotExportTest, charactersThatCannotBeInAFileNameAreReplaced)
{
	/* Including the two separators, which is what stops a typed name from
	   placing the file somewhere other than the export directory. */
	EXPECT_EQ(plotExport::sanitizeFileName("a/b\\c:d*e?f\"g<h>i|j"), "a_b_c_d_e_f_g_h_i_j");
}

TEST(PlotExportTest, controlCharactersAreReplacedAsWell)
{
	EXPECT_EQ(plotExport::sanitizeFileName(std::string("a\tb\nc")), "a_b_c");
}

TEST(PlotExportTest, surroundingSpacesAndDotsAreDropped)
{
	/* Windows trims these itself, so a name that keeps them comes back from
	   the dialog spelled differently from what was typed. */
	EXPECT_EQ(plotExport::sanitizeFileName("  shot..  "), "shot");
	EXPECT_EQ(plotExport::sanitizeFileName(".hidden"), "hidden");
}

TEST(PlotExportTest, aNameWithNothingUsableLeftFallsBack)
{
	EXPECT_EQ(plotExport::sanitizeFileName("   "), plotExport::defaultFileName);
	EXPECT_EQ(plotExport::sanitizeFileName(""), plotExport::defaultFileName);
	EXPECT_EQ(plotExport::sanitizeFileName("..."), plotExport::defaultFileName);
}

TEST(PlotExportTest, aNonAsciiNameIsKeptAsItIs)
{
	const std::string name = "曲线 截图";

	EXPECT_EQ(plotExport::sanitizeFileName(name), name);
}

TEST(PlotExportTest, oneImageKeepsThePlainName)
{
	plotExport::Settings settings;

	/* Even with incrementing on: a run of one has nothing to be told apart
	   from, and a bare name reads better in a directory listing. */
	EXPECT_TRUE(settings.incrementFileName);
	EXPECT_EQ(plotExport::imageFileName(settings, 0, 1), "MCUViewer_screenshot.png");
}

TEST(PlotExportTest, severalImagesAreNumberedFromOne)
{
	plotExport::Settings settings;

	EXPECT_EQ(plotExport::imageFileName(settings, 0, 3), "MCUViewer_screenshot_1.png");
	EXPECT_EQ(plotExport::imageFileName(settings, 1, 3), "MCUViewer_screenshot_2.png");
	EXPECT_EQ(plotExport::imageFileName(settings, 2, 3), "MCUViewer_screenshot_3.png");
}

TEST(PlotExportTest, theNumberGoesBeforeTheExtension)
{
	plotExport::Settings settings;
	settings.fileName = "run.png";

	EXPECT_EQ(plotExport::imageFileName(settings, 0, 2), "run_1.png");
	EXPECT_EQ(plotExport::imageFileName(settings, 1, 2), "run_2.png");
}

TEST(PlotExportTest, withoutIncrementingEveryImageSharesOneName)
{
	plotExport::Settings settings;
	settings.incrementFileName = false;

	/* The last one written is the one that survives, which is what turning the
	   option off asks for. */
	EXPECT_EQ(plotExport::imageFileName(settings, 0, 3), "MCUViewer_screenshot.png");
	EXPECT_EQ(plotExport::imageFileName(settings, 2, 3), "MCUViewer_screenshot.png");
}

TEST(PlotExportTest, aDirectoryIsJoinedWithOneSeparator)
{
	EXPECT_EQ(plotExport::joinPath("shots/", "a.png"), "shots/a.png");
	EXPECT_EQ(plotExport::joinPath("shots", "a.png"), "shots/a.png");
	EXPECT_EQ(plotExport::joinPath("shots\\", "a.png"), "shots\\a.png");
	EXPECT_EQ(plotExport::joinPath("", "a.png"), "a.png");
}

TEST(PlotExportTest, thePathOfAnImageUsesBothParts)
{
	plotExport::Settings settings;
	settings.directory = "D:/shots";

	EXPECT_EQ(plotExport::imagePath(settings, 0, 2), "D:/shots/MCUViewer_screenshot_1.png");
	EXPECT_EQ(plotExport::imagePath(settings, 1, 2), "D:/shots/MCUViewer_screenshot_2.png");
}

TEST(PlotExportTest, anEmptyDirectoryLeavesABareFileName)
{
	plotExport::Settings settings;
	settings.directory = "";

	/* The dialog then decides, or the file lands in the working directory. */
	EXPECT_EQ(plotExport::imagePath(settings, 0, 1), "MCUViewer_screenshot.png");
}
