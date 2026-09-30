#include "PlotExport.hpp"

#include <cctype>

const char* const plotExport::defaultFileName = "MCUViewer_screen";

std::string plotExport::sanitizeFileName(const std::string& name)
{
	std::string cleaned;
	cleaned.reserve(name.size());

	for (const char character : name)
	{
		const unsigned char value = static_cast<unsigned char>(character);

		/* The characters Windows rejects outright, the two separators, and the
		   control range. Replacing the separators is what keeps a typed name
		   from walking out of the export directory. */
		const bool forbidden = value < 0x20 || character == '<' || character == '>' || character == ':' || character == '"' ||
							   character == '/' || character == '\\' || character == '|' || character == '?' || character == '*';

		cleaned.push_back(forbidden ? '_' : character);
	}

	/* Windows drops trailing dots and spaces from a name, so a name that ends
	   in one would come back from the dialog spelled differently. */
	const size_t lastKept = cleaned.find_last_not_of(" .");
	cleaned.erase(lastKept == std::string::npos ? 0 : lastKept + 1);

	/* A leading dot would hide the file, and a leading space is dropped as
	   well, so both go before the name is measured again. */
	const size_t firstKept = cleaned.find_first_not_of(" .");

	if (firstKept == std::string::npos)
		return defaultFileName;

	cleaned.erase(0, firstKept);

	return cleaned;
}

std::string plotExport::withPngExtension(const std::string& name)
{
	if (name.size() >= 4)
	{
		std::string tail = name.substr(name.size() - 4);

		for (char& character : tail)
			character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

		if (tail == ".png")
			return name;
	}

	return name + ".png";
}

std::string plotExport::imageFileName(const Settings& settings, size_t index, size_t count)
{
	const std::string base = withPngExtension(sanitizeFileName(settings.fileName));

	if (count <= 1 || !settings.incrementFileName)
		return base;

	const std::string stem = base.substr(0, base.size() - 4);

	/* Numbering starts at one, so the first image of a run is "name_1.png".
	   That is the number a person reading the directory expects to be first. */
	return stem + "_" + std::to_string(index + 1) + ".png";
}

std::string plotExport::joinPath(const std::string& directory, const std::string& fileName)
{
	if (directory.empty())
		return fileName;

	std::string joined = directory;

	if (const char last = joined.back(); last != '/' && last != '\\')
		joined.push_back('/');

	return joined + fileName;
}

std::string plotExport::imagePath(const Settings& settings, size_t index, size_t count)
{
	return joinPath(settings.directory, imageFileName(settings, index, count));
}
