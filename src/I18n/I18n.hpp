#ifndef _I18N_HPP
#define _I18N_HPP

#include <cstddef>
#include <string>

/* The language the interface is drawn in.
 *
 * Every piece of text the user reads goes through `tr`, which is handed the
 * English that was written at the call site and returns what to draw instead.
 * Writing the English at the call site rather than a key means the source
 * still reads as English, a missing entry shows the English rather than a
 * blank or a raw key, and the table is a plain list of pairs that anyone can
 * check without holding a key list in their head.
 *
 * What is not translated is the vocabulary that names a thing rather than
 * describing it: the probes (ST-LINK, J-Link), the file formats (ELF, CSV,
 * PNG), the protocols (SWO, MCP), and the parser names. A reader who knows
 * what a J-Link is does not want the word changed, and a translated one would
 * no longer match the documentation it was read from.
 */
namespace I18n
{
enum class Language
{
	English,
	Chinese
};

/* The setting is stored as a name rather than as the number the list above
   happens to give it, so that a config written by a build offering a third
   language cannot be read as one of these two. */
Language languageFromName(const std::string& name);
const char* languageToName(Language language);

/* Read by every `tr` call, written by the preferences dialog. `inline` gives
   it one definition across translation units, which matters because the file
   that sets it is not the file that reads it. */
inline Language currentLanguage = Language::English;

/**
 * @brief The text to draw in place of an English one
 *
 * @param english text as written at the call site
 * @return const char* the translation, or `english` when there is none
 */
const char* tr(const char* english);

/* One line of the table.
 *
 * Public so that the tests can walk the whole of it. What has to hold - the
 * Chinese differs from the English, the format specifiers survive, a window
 * title keeps its id - holds for every entry or for none, so checking a
 * handful that were picked by hand would prove nothing. */
struct Entry
{
	const char* english;
	const char* chinese;
};

/**
 * @brief The whole table, sorted by the English text
 *
 * @param count filled with the number of entries
 * @return const Entry* the first entry
 */
const Entry* entries(size_t& count);

/**
 * @brief Every string the table holds, both languages
 *
 * Used to decide which glyphs the font has to carry. Handing the font the
 * whole CJK range would add megabytes of texture for characters no label
 * uses; handing it the strings themselves costs nothing and cannot drift
 * away from the table.
 *
 * @return const std::string&
 */
const std::string& allText();

}  // namespace I18n

#endif
