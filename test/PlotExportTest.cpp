#include <gtest/gtest.h>

#include <string>
#include <vector>

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

TEST(PlotExportTest, aSingleTargetIsTheNameOnItsOwn)
{
	plotExport::Settings settings;
	settings.directory = "D:/shots";

	const std::vector<std::string> targets = plotExport::targetPaths(settings, 1);

	ASSERT_EQ(targets.size(), 1);
	EXPECT_EQ(targets.at(0), "D:/shots/MCUViewer_screenshot.png");
}

TEST(PlotExportTest, aLongRunIsRepresentedByItsFirstTwo)
{
	plotExport::Settings settings;
	settings.directory = "D:/shots";

	/* The dialog prints these and says how many there are, so a run of twenty
	   does not grow the list it has to read. */
	const std::vector<std::string> targets = plotExport::targetPaths(settings, 20);

	ASSERT_EQ(targets.size(), 2);
	EXPECT_EQ(targets.at(0), "D:/shots/MCUViewer_screenshot_1.png");
	EXPECT_EQ(targets.at(1), "D:/shots/MCUViewer_screenshot_2.png");
}

TEST(PlotExportTest, aRunOfTwoIsShownInFull)
{
	plotExport::Settings settings;

	const std::vector<std::string> targets = plotExport::targetPaths(settings, 2);

	ASSERT_EQ(targets.size(), 2);
	EXPECT_EQ(targets.at(0), "MCUViewer_screenshot_1.png");
	EXPECT_EQ(targets.at(1), "MCUViewer_screenshot_2.png");
}

TEST(PlotExportTest, nothingToWriteHasNoTargets)
{
	plotExport::Settings settings;

	EXPECT_TRUE(plotExport::targetPaths(settings, 0).empty());
}

TEST(PlotExportTest, targetsFollowTheNameThatIsBeingTyped)
{
	plotExport::Settings settings;
	settings.directory = "D:/shots";
	settings.fileName = "my plots";

	/* The dialog calls this on every frame, so a half typed name has to come
	   back cleaned rather than throw or keep the forbidden characters. */
	const std::vector<std::string> targets = plotExport::targetPaths(settings, 3);

	ASSERT_EQ(targets.size(), 2);
	EXPECT_EQ(targets.at(0), "D:/shots/my plots_1.png");
}

TEST(PlotExportTest, aRunWithoutIncrementingShowsTheSameTargetTwice)
{
	plotExport::Settings settings;
	settings.incrementFileName = false;

	/* The dialog reads the repeat as the warning it is: every plot goes to one
	   file and only the last one is left. */
	const std::vector<std::string> targets = plotExport::targetPaths(settings, 4);

	ASSERT_EQ(targets.size(), 2);
	EXPECT_EQ(targets.at(0), targets.at(1));
}
