#include "McpProtocol.hpp"

namespace mcp
{
	namespace
	{
		/* Written for a model that has to decide what to call next, so the order of
		   the calls and the rule that matters most come first. */
		constexpr const char* instructionsText =
			"This server drives a live microcontroller debugging session. "
			"A variable is only read from the target while it belongs to a plot inside the active group, "
			"so add_variable_to_plot is the step that actually starts collection. "
			"A typical session is: set_debug_probe, configure_probe, set_elf_path, add_variable, add_plot, "
			"add_variable_to_plot, start_acquisition, then get_variable_value, get_variable_stats or get_curve_data. "
			"Writes to the target require the API write permission to be enabled in the application preferences. "
			"Call get_instance_info to see which subsystems this build provides.";
	}  // namespace

	bool isSupportedProtocolVersion(const std::string& version)
	{
		return version == "2025-06-18" || version == "2025-03-26" || version == "2024-11-05";
	}

	Json toolFailure(const std::string& message)
	{
		return Json::object({{"content", Json::array({Json::object({{"type", "text"}, {"text", message}})})},
							 {"isError", true}});
	}

	Json toolSuccess(const Json& payload)
	{
		return Json::object({{"content", Json::array({Json::object({{"type", "text"}, {"text", payload.dump()}})})},
							 {"isError", false}});
	}

	Protocol::Protocol(const ToolRegistry* registry, ToolInvoker invoke, VersionProvider version) : registry(registry), invoke(std::move(invoke)), version(std::move(version))
	{
	}

	Json Protocol::wrappedResult(const Json& id, const Json& result)
	{
		return Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
	}

	Json Protocol::wrappedError(const Json& id, int code, const std::string& message)
	{
		return Json::object({{"jsonrpc", "2.0"},
							 {"id", id},
							 {"error", Json::object({{"code", code}, {"message", message}})}});
	}

	Json Protocol::buildInitializeResult(const Json& params) const
	{
		/* A client that asks for a revision this build does not know is answered
		   with the newest one it does know, which is what the revision rule of the
		   protocol asks for. */
		std::string requested = latestProtocolVersion;

		if (params.contains("protocolVersion") && params.at("protocolVersion").is_string())
			requested = params.at("protocolVersion").get<std::string>();

		std::string applicationVersion = "unknown";

		if (version)
			applicationVersion = version();

		return Json::object({{"protocolVersion", isSupportedProtocolVersion(requested) ? requested : std::string(latestProtocolVersion)},
							 {"capabilities", Json::object({{"tools", Json::object({{"listChanged", false}})}})},
							 {"serverInfo", Json::object({{"name", serverName}, {"version", applicationVersion}})},
							 {"instructions", instructionsText}});
	}

	Json Protocol::handleRequest(const std::string& method, const Json& params)
	{
		if (method == "initialize")
			return buildInitializeResult(params);

		if (method == "ping")
			return Json::object({});

		if (method == "tools/list")
			return registry->listTools();

		if (method == "tools/call")
		{
			if (!params.contains("name") || !params.at("name").is_string())
				throw JsonRpcError(JsonRpcError::invalidParams, "Field 'name' is required.");

			if (invoke)
				return invoke(params.at("name").get<std::string>(), argumentsOf(params));

			throw JsonRpcError(JsonRpcError::internalError, "This server cannot run tools.");
		}

		/* Advertised as unavailable, but answering with an empty list keeps clients
		   that probe unconditionally from reporting a protocol failure. */
		if (method == "resources/list")
			return Json::object({{"resources", Json::array()}});

		if (method == "prompts/list")
			return Json::object({{"prompts", Json::array()}});

		if (method == "logging/setLevel")
			return Json::object({});

		throw JsonRpcError(JsonRpcError::methodNotFound, "Unknown method: '" + method + "'.");
	}

	Json Protocol::handleMessage(const Json& message)
	{
		/* Only an object can carry the id of a request. Anything else, a batch
		   array or a bare value, is a malformed message and gets an error answer
		   with a null id rather than being treated as a notification that must
		   stay unanswered. */
		const bool isObject = message.is_object();
		const bool notification = isObject && (!message.contains("id") || message.at("id").is_null());

		Json id;

		if (isObject && message.contains("id"))
			id = message.at("id");

		try
		{
			if (message.is_array())
				throw JsonRpcError(JsonRpcError::invalidRequest, "Batched requests are not supported by this protocol revision.");

			if (!message.is_object())
				throw JsonRpcError(JsonRpcError::invalidRequest, "A JSON-RPC message must be an object.");

			if (!message.contains("method") || !message.at("method").is_string())
			{
				/* A reply to something this server sent. There is nothing to answer. */
				if (message.contains("result") || message.contains("error"))
					return Json();

				throw JsonRpcError(JsonRpcError::invalidRequest, "Missing 'method'.");
			}

			const std::string method = message.at("method").get<std::string>();

			if (method.rfind("notifications/", 0) == 0)
				return Json();

			const Json params = message.contains("params") && message.at("params").is_object() ? message.at("params") : Json::object({});

			const Json result = handleRequest(method, params);

			if (notification)
				return Json();

			return wrappedResult(id, result);
		}
		catch (const JsonRpcError& error)
		{
			if (notification)
				return Json();

			return wrappedError(id, error.getCode(), error.what());
		}
		catch (const std::exception& error)
		{
			if (notification)
				return Json();

			return wrappedError(id, JsonRpcError::internalError, error.what());
		}
	}
}  // namespace mcp
