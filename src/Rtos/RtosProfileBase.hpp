#ifndef _RTOSPROFILEBASE_HPP
#define _RTOSPROFILEBASE_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "IElfParser.hpp"
#include "RtosLayout.hpp"
#include "Variable.hpp"

/*
 * What one RTOS looks like from the outside.
 *
 * A profile is a table, not code: the symbol names a kernel keeps its state
 * in, the fields of its structures, and the direction its priority numbers
 * run. Everything above this layer - the sampler, the model, the views, the
 * diagnostic rules - is written against the table and never mentions a kernel
 * by name, which is what makes adding the second RTOS a matter of adding one
 * file.
 *
 * The entries are addressed by a key of this project's own choosing rather
 * than by the kernel's identifier. The sampler asks for `readyLists` and gets
 * whatever the profile decided that means, so a kernel that calls its ready
 * lists something else changes nothing above this layer.
 */
class RtosProfileBase
{
   public:
	enum class Type : uint8_t
	{
		/* Detection decides; used as a request, never as a result. */
		Auto = 0,
		FreeRtos = 1,
		Zephyr = 2,
	};

	static std::string nameOf(Type type);
	static std::string descriptionOf(Type type);
	static bool typeFromName(const std::string& name, Type& type);

	enum class Need
	{
		Required,
		Optional,
	};

	struct SymbolSpec
	{
		std::string key;
		std::string name;
		Need need = Need::Required;
		std::string purpose;
	};

	/*
	 * One field, named by the structure it lives in.
	 *
	 * `altNames` carries the names the field has had across kernel releases.
	 * The container pointer inside a list item is `pvContainer` up to FreeRTOS
	 * V10 and `pxContainer` from V11 on; the reader tries them in order and
	 * records which one answered, so a symbol file from either release reads
	 * correctly and the interface can say which name it used.
	 */
	struct FieldSpec
	{
		std::string key;
		std::string structName;
		std::vector<std::string> altNames;
		Need need = Need::Required;
		std::string purpose;
	};

	struct SymbolRef
	{
		bool resolved = false;
		uint32_t address = 0;
		Variable::Type type = Variable::Type::UNKNOWN;
	};

	struct FieldRef
	{
		bool resolved = false;
		uint32_t offset = 0;
		std::string resolvedName = "";
		/* True when the offset came from the profile's own table rather than
		   from debug information. The interface shows it as a fallback. */
		bool usedFallback = false;
	};

	/*
	 * One pass of a profile over a symbol file and a layout. Everything the
	 * reader needs is in here, so nothing above this layer has to ask a kernel
	 * what it calls its own fields.
	 */
	struct Resolution
	{
		std::map<std::string, SymbolRef> symbols;
		std::map<std::string, FieldRef> fields;

		std::vector<std::string> missingRequired;
		std::vector<std::string> missingOptional;
		std::vector<std::string> fallbackFields;

		bool complete() const
		{
			return missingRequired.empty();
		}

		bool anyFallbackOffset() const
		{
			return !fallbackFields.empty();
		}

		bool has(const std::string& symbolKey) const
		{
			const auto it = symbols.find(symbolKey);

			return it != symbols.end() && it->second.resolved;
		}

		uint32_t addressOf(const std::string& symbolKey) const
		{
			const auto it = symbols.find(symbolKey);

			return it == symbols.end() ? 0u : it->second.address;
		}

		bool offsetOf(const std::string& fieldKey, uint32_t& offset) const
		{
			const auto it = fields.find(fieldKey);

			if (it == fields.end() || !it->second.resolved)
				return false;

			offset = it->second.offset;

			return true;
		}

		/* A sentence for the interface and the log. Names what is missing and
		   what to do about it, rather than reporting a count. */
		std::string summary() const;
	};

	virtual ~RtosProfileBase() = default;

	virtual Type getType() const = 0;
	virtual std::string getDescription() const = 0;

	virtual const std::vector<SymbolSpec>& getSymbols() const = 0;
	virtual const std::vector<FieldSpec>& getFields() const = 0;

	/*
	 * FreeRTOS numbers priorities so that a larger number is the higher
	 * priority; Zephyr does the opposite. Lane order and every "higher than"
	 * comparison goes through this rather than through a raw value, so no
	 * reader has to know which kernel it is looking at.
	 */
	virtual bool higherNumberIsHigherPriority() const = 0;

	/*
	 * How long a task name can be, including the terminating zero.
	 *
	 * FreeRTOS takes it from `configMAX_TASK_NAME_LEN` and does not store it
	 * in the control block, so a reader that only has the structure layout
	 * cannot work it out. The profile states it instead of leaving a reader to
	 * guess a number that would then silently truncate or overrun a name.
	 */
	virtual uint32_t taskNameCapacity() const = 0;

	/*
	 * The layout to use when debug information cannot answer. Every entry in
	 * it is marked as a fallback, so a table built from it is never presented
	 * as a reading.
	 */
	virtual RtosLayoutTable getFallbackLayout() const = 0;

	Resolution resolve(const IElfParser::SymbolMap& available, const IRtosLayout& layout) const;
};

#endif
