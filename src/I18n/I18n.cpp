#include "I18n.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace I18n
{
/* The pairs themselves, generated from the dictionary. Placed inside the
   namespace rather than at file scope so that the array and the type it holds
   are looked up the same way. */
#include "I18nTable.inc"

namespace
{
/* Two overloads rather than a lambda so that the comparison is written once:
   the value being searched for is a plain `const char*` and the elements are
   entries, and the algorithm asks for both orders. */
struct ByEnglish
{
	bool operator()(const Entry& entry, const char* english) const
	{
		return std::strcmp(entry.english, english) < 0;
	}

	bool operator()(const char* english, const Entry& entry) const
	{
		return std::strcmp(english, entry.english) < 0;
	}
};

constexpr size_t tableSize = sizeof(table) / sizeof(table[0]);

/* Bisects rather than walks the table. Every text on the screen comes through
   here once per frame, and there are a few hundred of them; a linear scan
   would be a few hundred thousand comparisons a second to save nothing. */
const Entry* find(const char* english)
{
	const Entry* beginning = table;
	const Entry* end = table + tableSize;

	const Entry* found = std::lower_bound(beginning, end, english, ByEnglish{});

	if (found == end || std::strcmp(found->english, english) != 0)
		return nullptr;

	return found;
}
}  // namespace

const Entry* entries(size_t& count)
{
	count = tableSize;
	return table;
}

Language languageFromName(const std::string& name)
{
	std::string lowered;
	lowered.reserve(name.size());

	for (const char character : name)
		lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));

	/* Anything unrecognised falls back to English rather than to Chinese: a
	   config written by a build that offers a language this one does not know
	   should show something the reader can work from. */
	return lowered == "chinese" ? Language::Chinese : Language::English;
}

const char* languageToName(Language language)
{
	return language == Language::Chinese ? "chinese" : "english";
}

const char* tr(const char* english)
{
	if (english == nullptr || currentLanguage == Language::English)
		return english;

	const Entry* entry = find(english);

	/* Not in the table: show what the call site wrote rather than nothing, so
	   a string that was missed is visible as English instead of as a gap. */
	return entry != nullptr ? entry->chinese : english;
}

std::string windowTitle(const char* english)
{
	if (english == nullptr)
		return std::string();

	std::string title = tr(english);
	title += "###";
	title += english;
	return title;
}

const std::string& allText()
{
	/* Built once. The font is set up once as well, so the cost is paid at
	   startup and never again, which is why this is not worth caching in a
	   member somewhere. */
	static const std::string text = []()
	{
		std::string collected;

		for (size_t index = 0; index < tableSize; index++)
		{
			collected += table[index].english;
			collected += table[index].chinese;
		}

		return collected;
	}();

	return text;
}

}  // namespace I18n
