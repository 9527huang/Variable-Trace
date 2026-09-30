#ifndef _MCPPROTOCOL_HPP
#define _MCPPROTOCOL_HPP

#include <functional>
#include <string>

#include "McpToolRegistry.hpp"
#include "McpTypes.hpp"

namespace mcp
{
	/* True when this build can speak the requested protocol revision. */
	bool isSupportedProtocolVersion(const std::string& version);

	/*
	 * A tool ran but could not produce a result, for example because the requested
	 * variable does not exist. It travels as a successful call whose payload says
	 * it failed, which is what a model can read and correct. A malformed request
	 * instead raises a JsonRpcError, because the model cannot repair that by
	 * reading a tool result.
	 */
	Json toolFailure(const std::string& message);

	/* The result envelope of a call that produced a payload. */
	Json toolSuccess(const Json& payload);

	/*
	 * The JSON-RPC layer, with no sockets and no knowledge of the application.
	 *
	 * Keeping it separate from the transport is what makes the version
	 * negotiation, the error shapes and the notification rules testable on their
	 * own: a test hands it a registry and a function that answers a tool call, and
	 * reads the replies back.
	 */
	class Protocol
	{
	   public:
		/* Runs a tool and returns its envelope, either a toolSuccess or a
		   toolFailure. Throws JsonRpcError for a request the client got wrong. */
		using ToolInvoker = std::function<Json(const std::string& name, const Json& arguments)>;

		using VersionProvider = std::function<std::string()>;

		Protocol(const ToolRegistry* registry, ToolInvoker invoke, VersionProvider version);

		/*
		 * Handles one JSON-RPC message.
		 *
		 * A null return is not an error: it means the message requires no reply,
		 * which is the rule for every notification and for a message that answers
		 * something the server sent. Such a message must never be answered, not
		 * even when handling it fails.
		 */
		Json handleMessage(const Json& message);

		static Json wrappedResult(const Json& id, const Json& result);
		static Json wrappedError(const Json& id, int code, const std::string& message);

	   private:
		Json handleRequest(const std::string& method, const Json& params);
		Json buildInitializeResult(const Json& params) const;

	   private:
		const ToolRegistry* registry;
		ToolInvoker invoke;
		VersionProvider version;
	};
}  // namespace mcp

#endif
