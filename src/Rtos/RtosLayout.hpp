#ifndef _RTOSLAYOUT_HPP
#define _RTOSLAYOUT_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>

/*
 * Where the structure offsets come from.
 *
 * Offsets have to be read from debug information rather than written down.
 * Turning on the list integrity checking options adds bytes to the head of
 * `List_t` and `ListItem_t`, and every offset in the layout moves with them,
 * so a table copied out of one build reads the wrong bytes on the next.
 *
 * The interface exists so that a profile does not care which reader answers.
 * `RtosLayoutTable` is the implementation that can be built and tested without
 * a symbol file at all.
 */
class IRtosLayout
{
   public:
	virtual ~IRtosLayout() = default;

	/* Both answer false for a name the layout has nothing to say about. */
	virtual bool structSize(const std::string& structName, uint32_t& size) const = 0;
	virtual bool fieldOffset(const std::string& structName, const std::string& fieldName, uint32_t& offset) const = 0;

	/* True when the number this layout answers with was not read out of debug
	   information. A reader that always reads the real layout answers with a
	   constant false, which is the honest answer for it. */
	virtual bool isFallback(const std::string& structName, const std::string& fieldName) const = 0;
};

/*
 * A layout held in memory, every entry remembering where its number came from.
 *
 * The origin decides what the task table is allowed to claim. A `Measured`
 * offset was read out of the debug information of the symbol file the target
 * was built with; a `Fallback` offset is a number written into the profile that
 * happened to be right for one build. Presenting the second as the first is
 * exactly the kind of silent wrong answer this project keeps running into, so
 * the two are carried separately all the way to the interface.
 */
class RtosLayoutTable : public IRtosLayout
{
   public:
	enum class Origin
	{
		Measured,
		Fallback,
	};

	struct Entry
	{
		uint32_t value = 0;
		Origin origin = Origin::Fallback;
	};

	void addStruct(const std::string& structName, uint32_t size, Origin origin)
	{
		structs[structName] = Entry{size, origin};
	}

	void addField(const std::string& structName, const std::string& fieldName, uint32_t offset, Origin origin)
	{
		fields[keyOf(structName, fieldName)] = Entry{offset, origin};
	}

	bool structSize(const std::string& structName, uint32_t& size) const override
	{
		const auto it = structs.find(structName);

		if (it == structs.end())
			return false;

		size = it->second.value;

		return true;
	}

	bool fieldOffset(const std::string& structName, const std::string& fieldName, uint32_t& offset) const override
	{
		const auto it = fields.find(keyOf(structName, fieldName));

		if (it == fields.end())
			return false;

		offset = it->second.value;

		return true;
	}

	bool isFallback(const std::string& structName, const std::string& fieldName) const override
	{
		const auto it = fields.find(keyOf(structName, fieldName));

		if (it == fields.end())
			return true;

		return it->second.origin == Origin::Fallback;
	}

	std::optional<Origin> structOrigin(const std::string& structName) const
	{
		const auto it = structs.find(structName);

		if (it == structs.end())
			return std::nullopt;

		return it->second.origin;
	}

	std::optional<Origin> fieldOrigin(const std::string& structName, const std::string& fieldName) const
	{
		const auto it = fields.find(keyOf(structName, fieldName));

		if (it == fields.end())
			return std::nullopt;

		return it->second.origin;
	}

	/* True when the table holds nothing that was read from debug information.
	   A table built entirely from fallbacks has to be marked as such wherever
	   its numbers are shown. */
	bool isAllFallback() const
	{
		for (const auto& entry : structs)
		{
			if (entry.second.origin == Origin::Measured)
				return false;
		}

		for (const auto& entry : fields)
		{
			if (entry.second.origin == Origin::Measured)
				return false;
		}

		return true;
	}

	size_t structCount() const
	{
		return structs.size();
	}

	size_t fieldCount() const
	{
		return fields.size();
	}

   private:
	static std::string keyOf(const std::string& structName, const std::string& fieldName)
	{
		return structName + "." + fieldName;
	}

	std::map<std::string, Entry> structs;
	std::map<std::string, Entry> fields;
};

#endif
