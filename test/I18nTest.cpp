#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "I18n.hpp"
#include "Plot.hpp"

/*
 * The interface can be read in English or in Chinese, and every label asks the
 * table for what to draw.
 *
 * What is worth checking is not that a handful of chosen labels read well - a
 * person does that by looking at the window. It is that the things the table
 * has to be true of hold for every line at once, because those are the ways a
 * translation goes wrong without saying so:
 *
 *   - a line that was never filled in, which leaves the English on screen
 *   - a line whose format specifiers were dropped or reordered, which turns a
 *     number into a stray word or crashes the printout
 *   - a window title that lost the id it keeps its saved position under
 *   - two lines sharing a key, where one of them silently never appears
 *
 * The tests below are written against the whole table for that reason.
 */

namespace
{
/* The entries that are the same in both languages on purpose.
 *
 * `English` names a language, and a language is written in itself - someone
 * looking for English among `中文` and `Deutsch` is looking for the word
 * `English`. `XY` names a kind of plot after the axes it draws, and the two
 * letters are the name.
 *
 * The remaining three are what the About panel shows under the title: the
 * product name, the author's name and the line that records which upstream
 * release this is built on. A name is not translated, and neither is the
 * attribution, so they read the same in either language.
 */
bool isDeliberatelyUnchanged(const char* english)
{
	return std::strcmp(english, "English") == 0 || std::strcmp(english, "XY") == 0 || std::strcmp(english, "Variable-Trace | ") == 0 ||
		   std::strcmp(english, "Variable-Trace") == 0 || std::strcmp(english, "by Piotr Wasilewski (klonyyy)") == 0 ||
		   std::strcmp(english, "based on MCUViewer 1.1.0 (GPLv3)") == 0;
}

/* The conversions a printf style format string carries, in order.
 *
 * Read as text rather than run through printf, because what has to hold is
 * that the translation asks for the same values in the same order - the
 * arguments come from the call site and the translated string is what reads
 * them.
 */
std::vector<std::string> conversions(const char* format)
{
	std::vector<std::string> found;

	for (const char* cursor = format; *cursor != '\0'; cursor++)
	{
		if (*cursor != '%')
			continue;

		const char* start = cursor;
		cursor++;

		while (*cursor == '-' || *cursor == '+' || *cursor == ' ' || *cursor == '#' || *cursor == '0' || *cursor == '.' || (*cursor >= '0' && *cursor <= '9') || *cursor == '*')
			cursor++;

		if (*cursor == '\0')
			break;

		found.emplace_back(start, cursor - start + 1);
	}

	return found;
}

class I18nTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		previous = I18n::currentLanguage;
		I18n::currentLanguage = I18n::Language::Chinese;
	}

	/* Put back, so that a test that changes the language cannot decide what
	   the next one sees. */
	void TearDown() override
	{
		I18n::currentLanguage = previous;
	}

private:
	I18n::Language previous = I18n::Language::English;
};
}  // namespace

TEST_F(I18nTest, everyLineHasAChineseThatDiffersFromItsEnglish)
{
	size_t count = 0;
	const I18n::Entry* table = I18n::entries(count);

	ASSERT_GT(count, 0u);

	for (size_t index = 0; index < count; index++)
	{
		const I18n::Entry& entry = table[index];

		if (isDeliberatelyUnchanged(entry.english))
			continue;

		EXPECT_STRNE(entry.english, entry.chinese) << "left in English: " << entry.english;
		EXPECT_GT(std::strlen(entry.chinese), 0u) << "left empty: " << entry.english;
	}
}

TEST_F(I18nTest, everyLineKeepsTheValuesItsEnglishAsksFor)
{
	size_t count = 0;
	const I18n::Entry* table = I18n::entries(count);

	for (size_t index = 0; index < count; index++)
	{
		const I18n::Entry& entry = table[index];

		const std::vector<std::string> wanted = conversions(entry.english);
		const std::vector<std::string> offered = conversions(entry.chinese);

		ASSERT_EQ(wanted.size(), offered.size()) << "a value went missing in: " << entry.english;

		for (size_t position = 0; position < wanted.size(); position++)
			EXPECT_EQ(wanted[position], offered[position]) << "position " << position << " of: " << entry.english;
	}
}

/* The docked windows, as the source names them when it opens them. Each is
   named in the layout file, and each is a pane the reader is meant to see
   translated. */
const char* const dockedWindows[] = {
	"Trace Viewer",
	"Trace Plots",
	"Var Viewer",
	"Plots",
	"Flash Output",
	"Recorder",
	"Statistics",
};

/* A name is stored without an id marker in it.
 *
 * The marker is added where the name is built, and only there. A translation
 * that carried one would come out with two, and ImGui cuts and hashes at the
 * first of them, so the second would be the one that counted - the identifier
 * would follow the language again, which is the whole thing the marker is
 * there to prevent.
 */
TEST_F(I18nTest, noStoredTextCarriesAnIdMarker)
{
	size_t count = 0;
	const I18n::Entry* table = I18n::entries(count);

	for (size_t index = 0; index < count; index++)
	{
		EXPECT_EQ(std::strstr(table[index].english, "###"), nullptr) << "a marker is stored in the table: " << table[index].english;
		EXPECT_EQ(std::strstr(table[index].chinese, "###"), nullptr) << "a marker is stored in the table: " << table[index].chinese;
	}
}

/* A window is opened with one name that does two jobs.
 *
 * What the reader sees in the title bar and the tab has to be the translation.
 * What the layout file files the window's dock and its size under has to be
 * the same whichever language is in force, or the layout saved in one language
 * would not describe the windows of the other and a pane would come up where
 * the other language had left it. ImGui cuts a name at `##` twice over - it
 * stops drawing there and it restarts its hash there - so one string can be
 * read as one thing and stored as another.
 */
TEST_F(I18nTest, aWindowNameReadsInTheLanguageAndIsFiledUnderOneKey)
{
	for (const char* title : dockedWindows)
	{
		const char* translated = I18n::tr(title);
		const std::string name = I18n::windowTitle(title);

		ASSERT_STRNE(translated, title) << "a docked window is not translated: " << title;

		/* Everything up to the marker is what gets drawn. */
		EXPECT_EQ(std::strncmp(name.c_str(), translated, std::strlen(translated)), 0) << title;
		EXPECT_EQ(name[std::strlen(translated)], '#') << title;

		/* And from the marker on it is the English, which no language moves. */
		EXPECT_EQ(name.substr(name.find("###")), std::string("###") + title) << title;
	}
}

/* The same window, named in both languages: the reader's half differs and the
   half the layout file keeps does not. */
TEST_F(I18nTest, theSameWindowIsFiledUnderTheSameKeyInBothLanguages)
{
	I18n::currentLanguage = I18n::Language::English;
	const std::string english = I18n::windowTitle("Var Viewer");

	I18n::currentLanguage = I18n::Language::Chinese;
	const std::string chinese = I18n::windowTitle("Var Viewer");

	EXPECT_NE(english, chinese);
	EXPECT_EQ(english.substr(english.find("###")), chinese.substr(chinese.find("###")));
}

/* The lookup bisects, so the table has to be in order. A line inserted in the
   wrong place makes every search past it miss. */
TEST_F(I18nTest, theTableIsSortedByItsEnglish)
{
	size_t count = 0;
	const I18n::Entry* table = I18n::entries(count);

	for (size_t index = 1; index < count; index++)
		EXPECT_LT(std::strcmp(table[index - 1].english, table[index].english), 0)
			<< "out of order: " << table[index - 1].english << " then " << table[index].english;
}

/* A key that appears twice would be answered by whichever copy the bisect
   happens to land on, and the other translation would never be seen. */
TEST_F(I18nTest, noEnglishTextAppearsTwice)
{
	size_t count = 0;
	const I18n::Entry* table = I18n::entries(count);

	for (size_t index = 1; index < count; index++)
		EXPECT_STRNE(table[index - 1].english, table[index].english) << "duplicated: " << table[index].english;
}

TEST_F(I18nTest, englishLeavesTheTextAlone)
{
	I18n::currentLanguage = I18n::Language::English;

	const char* text = "Save as..";

	EXPECT_EQ(I18n::tr(text), text);
}

TEST_F(I18nTest, chineseReplacesTheText)
{
	size_t count = 0;
	const I18n::Entry* table = I18n::entries(count);

	for (size_t index = 0; index < count; index++)
	{
		const I18n::Entry& entry = table[index];

		/* Asked for by the English the call site would have written, which is
		   what has to find it - not by the key the generator happened to
		   emit. */
		EXPECT_STREQ(I18n::tr(entry.english), entry.chinese) << "not found: " << entry.english;
	}
}

TEST_F(I18nTest, anUnknownTextComesBackUnchanged)
{
	const char* text = "a string that was never put in the table";

	EXPECT_EQ(I18n::tr(text), text);
}

/* Text that a value passes through twice - the id of a question box is
   translated at the call site and again where the box is drawn - has to
   survive the second pass. It does, because the Chinese is not a key. */
TEST_F(I18nTest, translatingATranslationLeavesItAlone)
{
	const char* once = I18n::tr("Preferences");

	EXPECT_STRNE(once, "Preferences");
	EXPECT_EQ(I18n::tr(once), once);
}

TEST_F(I18nTest, theLanguageNameRoundTrips)
{
	for (const I18n::Language language : {I18n::Language::English, I18n::Language::Chinese})
		EXPECT_EQ(I18n::languageFromName(I18n::languageToName(language)), language);
}

TEST_F(I18nTest, aStoredNameIsMatchedWhateverItsCase)
{
	EXPECT_EQ(I18n::languageFromName("chinese"), I18n::Language::Chinese);
	EXPECT_EQ(I18n::languageFromName("Chinese"), I18n::Language::Chinese);
	EXPECT_EQ(I18n::languageFromName("CHINESE"), I18n::Language::Chinese);

	/* A config written by a build that offers a language this one does not
	   know has to leave the interface readable. */
	EXPECT_EQ(I18n::languageFromName(""), I18n::Language::English);
	EXPECT_EQ(I18n::languageFromName("english"), I18n::Language::English);
	EXPECT_EQ(I18n::languageFromName("german"), I18n::Language::English);
}

/* The two names an axis falls back on - the time axis of a curve and the
   value axis every other kind carries - are interface text, so they follow
   the language. A label that was typed into the editor belongs to the plot
   and is drawn as written, whatever the language is.

   The wiring is what is checked rather than the wording: the plot has to ask
   the table, and the table is what decides what the words are. */
TEST_F(I18nTest, aPlotAxisAsksTheTableForItsAutomaticName)
{
	Plot plot("p");

	const std::string automaticX = plot.getEffectiveXAxisLabel();
	const std::string automaticY = plot.getEffectiveYAxisLabel();

	EXPECT_EQ(automaticX, I18n::tr("time[s]"));
	EXPECT_EQ(automaticY, I18n::tr("Value"));

	EXPECT_STRNE(automaticX.c_str(), "time[s]");
	EXPECT_STRNE(automaticY.c_str(), "Value");

	plot.setXAxisLabel("rpm");
	plot.setYAxisLabel("torque");

	EXPECT_EQ(plot.getEffectiveXAxisLabel(), "rpm");
	EXPECT_EQ(plot.getEffectiveYAxisLabel(), "torque");
}

/* The font is given this and only this to decide which glyphs to carry, so a
   line missing from it draws as blank boxes. */
TEST_F(I18nTest, everyLineReachesTheGlyphRangeText)
{
	const std::string& text = I18n::allText();

	size_t count = 0;
	const I18n::Entry* table = I18n::entries(count);

	for (size_t index = 0; index < count; index++)
	{
		EXPECT_NE(text.find(table[index].english), std::string::npos) << "missing: " << table[index].english;
		EXPECT_NE(text.find(table[index].chinese), std::string::npos) << "missing: " << table[index].chinese;
	}
}
