#include "GdbRtosLayout.hpp"

#include <algorithm>
#include <regex>

#include "ProcessHandler.hpp"

namespace
{
	/*
	 * One field line of a `ptype /o` dump. It looks like:
	 *
	 *   [offset comment] [width comment]    volatile StackType_t *pxTopOfStack;
	 *
	 * The comment before the declaration holds the offset and the width, and
	 * the declaration runs up to its semicolon. The declaration can be long
	 * and can carry brackets of its own, so only the offset is taken from the
	 * comment and the name is worked out from the declaration afterwards.
	 */
	const std::regex fieldLine(R"(/\*\s*(\d+)\s*\|\s*(\d+)\s*\*/\s*([^;]+);)");

	/* The line that closes a dump, carrying the total size in bytes. */
	const std::regex sizeLine(R"(total size \(bytes\):\s*(\d+))");

	/* The line that names it: type = struct tskTaskControlBlock { */
	const std::regex structLine(R"(type\s*=\s*(?:struct|union)\s+([A-Za-z_][A-Za-z0-9_]*))");

	std::string trimTrailing(std::string text)
	{
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\\'))
			text.pop_back();

		return text;
	}

	/* The name is the last word of the declaration, with the parts that are
	   not part of a name cut off. */
	std::string nameOf(const std::string& declaration)
	{
		const std::string trimmed = trimTrailing(declaration);
		const auto space = trimmed.find_last_of(" \t");

		std::string name = (space == std::string::npos) ? trimmed : trimmed.substr(space + 1);

		/* A pointer declaration puts the star against the name, so `struct
		   xLIST_ITEM *pxNext` ends in `*pxNext` and not in `pxNext`. */
		while (!name.empty() && name.front() == '*')
			name.erase(name.begin());

		/* An array extent is not part of the name, the same way it is not in a
		   symbol file. */
		const auto bracket = name.find('[');

		if (bracket != std::string::npos)
			name.erase(bracket);

		/* A bit field carries its width after the name. */
		const auto colon = name.find(':');

		if (colon != std::string::npos)
			name.erase(colon);

		return name;
	}
}  // namespace

std::vector<std::string> GdbRtosLayout::structuresOf(const RtosProfileBase& profile)
{
	std::vector<std::string> names;

	for (const RtosProfileBase::FieldSpec& field : profile.getFields())
	{
		if (field.structName.empty())
			continue;

		if (std::find(names.begin(), names.end(), field.structName) == names.end())
			names.push_back(field.structName);
	}

	return names;
}

bool GdbRtosLayout::parseStructDump(const std::string& dump, const std::string& structureName, RtosLayoutTable& table)
{
	std::smatch match;

	/* The dump has to name a structure at all, or there is nothing to file.
	   What it names it is not used: see the note in the header. */
	if (!std::regex_search(dump, match, structLine))
		return false;

	if (structureName.empty())
		return false;

	if (std::regex_search(dump, match, sizeLine))
	{
		const uint32_t size = static_cast<uint32_t>(std::stoul(match[1].str()));

		if (size > 0)
			table.addStruct(structureName, size, RtosLayoutTable::Origin::Measured);
	}

	for (auto it = std::sregex_iterator(dump.begin(), dump.end(), fieldLine); it != std::sregex_iterator(); ++it)
	{
		const std::smatch& line = *it;
		const uint32_t offset = static_cast<uint32_t>(std::stoul(line[1].str()));
		const std::string fieldName = nameOf(line[3].str());

		if (fieldName.empty())
			continue;

		table.addField(structureName, fieldName, offset, RtosLayoutTable::Origin::Measured);
	}

	return true;
}

RtosLayoutTable GdbRtosLayout::read(const std::string& gdbCommand, const std::string& elfPath, const RtosProfileBase& profile, spdlog::logger* logger)
{
	/*
	 * Started from the profile's own table rather than from nothing.
	 *
	 * Not every field can be measured. The two inside the union at the head of
	 * a queue are the standing example: `ptype /o` shows the union but not
	 * what is in it, so no amount of reading will produce those offsets. A
	 * table that held only what was measured would leave them unresolved, and
	 * a field with no offset is a feature that silently does not work.
	 *
	 * So the profile's numbers are laid down first and the measurements
	 * overwrite them. What ends up measured is marked as measured and what
	 * ends up written down stays marked as written down, which is what lets
	 * the interface say which is which.
	 */
	RtosLayoutTable table = profile.getFallbackLayout();

	if (gdbCommand.empty() || elfPath.empty())
		return table;

	ProcessHandler process;

	/* One session, every structure asked over it. Opening gdb once per
	   structure would multiply a startup cost already measured in seconds. */
	if (process.executeCmd(gdbCommand + " --interpreter=mi " + elfPath, "(gdb)").empty())
	{
		if (logger != nullptr)
			logger->warn("The kernel structure layout could not be read: {} did not start on {}.", gdbCommand, elfPath);

		return table;
	}

	for (const std::string& structure : structuresOf(profile))
	{
		const std::string dump = process.executeCmd("ptype /o " + structure + "\n", "(gdb)");

		if (parseStructDump(dump, structure, table))
			continue;

		/* Not an error. A structure the debug information does not describe
		   leaves its fields to the profile's own numbers, which are marked as
		   fallbacks and shown as such. */
		if (logger != nullptr)
			logger->debug("The debug information does not describe {}; the profile's own offsets are used for it.", structure);
	}

	process.closePipes();

	return table;
}
