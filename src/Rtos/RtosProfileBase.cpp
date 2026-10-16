#include "RtosProfileBase.hpp"

#include <algorithm>
#include <cctype>

std::string RtosProfileBase::nameOf(Type type)
{
	switch (type)
	{
		case Type::FreeRtos:
			return "freertos";

		case Type::Zephyr:
			return "zephyr";

		case Type::Auto:
		default:
			return "auto";
	}
}

std::string RtosProfileBase::descriptionOf(Type type)
{
	switch (type)
	{
		case Type::FreeRtos:
			return "FreeRTOS, read through the kernel's own lists";

		case Type::Zephyr:
			return "Zephyr, read through the kernel's own lists";

		case Type::Auto:
		default:
			return "Pick the kernel from the symbol file";
	}
}

bool RtosProfileBase::typeFromName(const std::string& name, Type& type)
{
	std::string lowered;
	lowered.reserve(name.size());

	for (char c : name)
		lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

	if (lowered == "freertos")
	{
		type = Type::FreeRtos;

		return true;
	}

	if (lowered == "zephyr")
	{
		type = Type::Zephyr;

		return true;
	}

	if (lowered == "auto")
	{
		type = Type::Auto;

		return true;
	}

	return false;
}

std::string RtosProfileBase::Resolution::summary() const
{
	std::string text;

	if (complete())
		text = "every required symbol and offset resolved";
	else
		text = "missing " + std::to_string(missingRequired.size()) + " required: ";

	if (!complete())
	{
		for (size_t i = 0; i < missingRequired.size(); i++)
			text += (i == 0 ? "" : ", ") + missingRequired[i];
	}

	if (!missingOptional.empty())
		text += (complete() ? "" : "; ") + std::string("missing optional: ") + std::to_string(missingOptional.size());

	if (!fallbackFields.empty())
		text += "; " + std::to_string(fallbackFields.size()) + " offset(s) came from the built-in table rather than from debug information";

	return text;
}

RtosProfileBase::Resolution RtosProfileBase::resolve(const IElfParser::SymbolMap& available, const IRtosLayout& layout) const
{
	Resolution result;

	for (const SymbolSpec& spec : getSymbols())
	{
		/* A spec with no name is how a profile says "this kernel has no such
		   thing". It is neither present nor missing. */
		if (spec.name.empty())
			continue;

		const auto it = available.find(spec.name);

		if (it != available.end())
		{
			SymbolRef ref;
			ref.resolved = true;
			ref.address = it->second.address;
			ref.type = it->second.type;
			result.symbols[spec.key] = ref;

			continue;
		}

		const std::string described = spec.name + " (" + spec.purpose + ")";

		if (spec.need == Need::Required)
			result.missingRequired.push_back(described);
		else
			result.missingOptional.push_back(described);
	}

	for (const FieldSpec& spec : getFields())
	{
		FieldRef ref;

		for (const std::string& candidate : spec.altNames)
		{
			uint32_t offset = 0;

			if (!layout.fieldOffset(spec.structName, candidate, offset))
				continue;

			ref.resolved = true;
			ref.offset = offset;
			ref.resolvedName = candidate;
			ref.usedFallback = layout.isFallback(spec.structName, candidate);

			if (ref.usedFallback)
				result.fallbackFields.push_back(spec.key);

			break;
		}

		result.fields[spec.key] = ref;

		if (ref.resolved)
			continue;

		const std::string fieldName = spec.altNames.empty() ? std::string("?") : spec.altNames.front();
		const std::string described = spec.structName + "." + fieldName + " (" + spec.purpose + ")";

		if (spec.need == Need::Required)
			result.missingRequired.push_back(described);
		else
			result.missingOptional.push_back(described);
	}

	return result;
}
