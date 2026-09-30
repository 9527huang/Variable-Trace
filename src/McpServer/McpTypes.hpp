#ifndef _MCPTYPES_HPP
#define _MCPTYPES_HPP

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

/*
 * Shared vocabulary of the MCP server.
 *
 * The tool descriptions, argument names and enum spellings are the public
 * contract of this program, so they are written out literally in the tool
 * registration files rather than composed from constants.
 */
namespace mcp
{
	using Json = nlohmann::json;

	/* Server name and version reported to clients during initialize. */
	static constexpr const char* serverName = "Variable-Trace";

	/* Latest protocol revision this implementation understands. */
	static constexpr const char* latestProtocolVersion = "2025-06-18";

	/*
	 * A tool ran but could not produce a result, for example because the
	 * requested variable does not exist. The message is handed to the client as
	 * a tool result with isError set, which is what a model can read and react
	 * to. Permanent failures that the model cannot fix by retrying belong here.
	 */
	class ToolError : public std::runtime_error
	{
	   public:
		explicit ToolError(const std::string& message) : std::runtime_error(message)
		{
		}
	};

	/*
	 * The request itself was malformed, or asked for something this server does
	 * not implement. Reported as a JSON-RPC error object.
	 */
	class JsonRpcError : public std::runtime_error
	{
	   public:
		JsonRpcError(int code, const std::string& message) : std::runtime_error(message), code(code)
		{
		}

		int getCode() const
		{
			return code;
		}

		static constexpr int parseError = -32700;
		static constexpr int invalidRequest = -32600;
		static constexpr int methodNotFound = -32601;
		static constexpr int invalidParams = -32602;
		static constexpr int internalError = -32603;

	   private:
		int code;
	};

	/* Arguments of tools/call. An omitted field is an empty object, which is what
	   clients send for tools that take no parameters. */
	Json argumentsOf(const Json& params);

	std::string requireString(const Json& arguments, const std::string& field);
	std::optional<std::string> optionalString(const Json& arguments, const std::string& field);
	std::optional<double> optionalNumber(const Json& arguments, const std::string& field);
	std::optional<int64_t> optionalInteger(const Json& arguments, const std::string& field);
	std::optional<bool> optionalBool(const Json& arguments, const std::string& field);

	/* Bitmasks are documented in hexadecimal, so a decimal string and a
	   "0x"-prefixed string both have to be understood, which optionalInteger
	   cannot do because it routes through a floating point conversion. */
	std::optional<uint64_t> optionalUnsigned(const Json& arguments, const std::string& field);

	/* Accepts a string array as well as a single string, because several tools
	   accept a batch form and it is cheaper to normalise once here. */
	std::vector<std::string> stringArray(const Json& value, const std::string& field);

	bool has(const Json& arguments, const std::string& field);
}  // namespace mcp

#endif
