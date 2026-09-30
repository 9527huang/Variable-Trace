#include "McpTypes.hpp"

namespace mcp
{
	namespace
	{
		/* The error text deliberately names the offending field, because a model
		   correcting its own call only has this message to work with. */
		[[noreturn]] void reportWrongType(const std::string& field, const char* expected)
		{
			throw ToolError("Field '" + field + "' must be " + std::string(expected) + ".");
		}
	}  // namespace

	Json argumentsOf(const Json& params)
	{
		if (!params.is_object())
			return Json::object();

		auto entry = params.find("arguments");

		if (entry == params.end() || entry->is_null())
			return Json::object();

		if (!entry->is_object())
			throw JsonRpcError(JsonRpcError::invalidParams, "Field 'arguments' must be an object.");

		return *entry;
	}

	bool has(const Json& arguments, const std::string& field)
	{
		auto entry = arguments.find(field);
		return entry != arguments.end() && !entry->is_null();
	}

	std::string requireString(const Json& arguments, const std::string& field)
	{
		auto entry = arguments.find(field);

		if (entry == arguments.end() || entry->is_null())
			throw ToolError("Field '" + field + "' is required.");

		if (!entry->is_string())
			reportWrongType(field, "a string");

		const std::string value = entry->get<std::string>();

		if (value.empty())
			throw ToolError("Field '" + field + "' must not be empty.");

		return value;
	}

	std::optional<std::string> optionalString(const Json& arguments, const std::string& field)
	{
		auto entry = arguments.find(field);

		if (entry == arguments.end() || entry->is_null())
			return std::nullopt;

		if (!entry->is_string())
			reportWrongType(field, "a string");

		return entry->get<std::string>();
	}

	std::optional<double> optionalNumber(const Json& arguments, const std::string& field)
	{
		auto entry = arguments.find(field);

		if (entry == arguments.end() || entry->is_null())
			return std::nullopt;

		if (entry->is_number())
			return entry->get<double>();

		/* Clients frequently send numbers as strings when they come out of a
		   form field, so the conversion is attempted instead of refusing. */
		if (entry->is_string())
		{
			try
			{
				return std::stod(entry->get<std::string>());
			}
			catch (const std::exception&)
			{
				reportWrongType(field, "a number");
			}
		}

		reportWrongType(field, "a number");
	}

	std::optional<int64_t> optionalInteger(const Json& arguments, const std::string& field)
	{
		std::optional<double> number = optionalNumber(arguments, field);

		if (!number.has_value())
			return std::nullopt;

		return static_cast<int64_t>(*number);
	}

	std::optional<bool> optionalBool(const Json& arguments, const std::string& field)
	{
		auto entry = arguments.find(field);

		if (entry == arguments.end() || entry->is_null())
			return std::nullopt;

		if (entry->is_boolean())
			return entry->get<bool>();

		/* 0/1 is what the documented signatures use, so both spellings are taken. */
		if (entry->is_number())
			return entry->get<double>() != 0.0;

		if (entry->is_string())
		{
			const std::string value = entry->get<std::string>();

			if (value == "true" || value == "1")
				return true;

			if (value == "false" || value == "0")
				return false;
		}

		reportWrongType(field, "a boolean or 0/1");
	}

	std::optional<uint64_t> optionalUnsigned(const Json& arguments, const std::string& field)
	{
		auto entry = arguments.find(field);

		if (entry == arguments.end() || entry->is_null())
			return std::nullopt;

		if (entry->is_number_unsigned())
			return entry->get<uint64_t>();

		if (entry->is_number_integer())
		{
			const int64_t value = entry->get<int64_t>();

			if (value < 0)
				reportWrongType(field, "a non-negative integer");

			return static_cast<uint64_t>(value);
		}

		if (entry->is_string())
		{
			const std::string text = entry->get<std::string>();

			try
			{
				size_t consumed = 0;
				const uint64_t value = std::stoull(text, &consumed, 0);

				/* A trailing character means the text was not a plain number, and
				   silently keeping the parsed prefix would apply a wrong mask. */
				if (consumed == text.size())
					return value;
			}
			catch (const std::exception&)
			{
			}

			reportWrongType(field, "a non-negative integer");
		}

		reportWrongType(field, "a non-negative integer");
	}

	std::vector<std::string> stringArray(const Json& value, const std::string& field)
	{
		std::vector<std::string> result;

		if (value.is_string())
		{
			const std::string single = value.get<std::string>();

			if (single.empty())
				throw ToolError("Field '" + field + "' must not be empty.");

			result.push_back(single);
			return result;
		}

		if (!value.is_array())
			reportWrongType(field, "an array of strings");

		for (const auto& element : value)
		{
			if (!element.is_string())
				reportWrongType(field, "an array of strings");

			result.push_back(element.get<std::string>());
		}

		if (result.empty())
			throw ToolError("Field '" + field + "' must not be empty.");

		return result;
	}
}  // namespace mcp
