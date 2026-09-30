#ifndef _MCPSERVER_HPP
#define _MCPSERVER_HPP

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "McpBridge.hpp"
#include "McpContext.hpp"
#include "McpProtocol.hpp"
#include "McpToolRegistry.hpp"
#include "McpTypes.hpp"
#include "spdlog/spdlog.h"

/* Kept out of the header so that the HTTP library stays an implementation
   detail of this translation unit. */
namespace httplib
{
	class Server;
}

namespace mcp
{
	/*
	 * Exposes the application to an AI agent over the Model Context Protocol.
	 *
	 * The transport is HTTP on the loopback interface. Two flavours are served
	 * from the same instance: the current streamable transport on /mcp, and the
	 * older HTTP with server sent events transport on /sse + /messages, which
	 * older clients still expect. Both end up in the same protocol handler.
	 */
	class Server
	{
	   public:
		static constexpr uint16_t defaultPort = 7777;

		/* How far the search walks forward when the preferred port is taken. */
		static constexpr uint16_t portScanRange = 20;

		Server(McpContext* context, Bridge* bridge, spdlog::logger* logger);
		~Server();

		Server(const Server&) = delete;
		Server& operator=(const Server&) = delete;

		/* Binds the first free port at or above preferredPort and starts
		   listening. Returns false when disabled or when no port could be bound;
		   the reason is then available through getLastError(). */
		bool start(bool enabled, uint16_t preferredPort);

		void stop();

		/* Loading a project can change the connection settings, so the listener is
		   rebuilt afterwards. */
		void restart(bool enabled, uint16_t preferredPort);

		bool isRunning() const;
		uint16_t getPort() const;

		/* http://127.0.0.1:<port>/mcp, the address an MCP client is configured
		   with. Empty while the server is not running. */
		std::string getEndpoint() const;

		std::string getLastError() const;

		size_t getToolCount() const;

		/* Protocol handling without sockets, so that it can be exercised by the
		   tests. Returns a null value for messages that require no reply. */
		Json handleMessage(const Json& message);

		/* Runs a tool and returns its result envelope, with isError set when the
		   tool itself could not do its job. Throws JsonRpcError only for a call
		   the client got wrong. */
		Json callTool(const std::string& name, const Json& arguments);

		const ToolRegistry& getRegistry() const
		{
			return registry;
		}

	   private:
		struct Session;

		void registerTools();
		void registerRoutes(httplib::Server& server);

		std::shared_ptr<Session> openSession(const std::string& sessionId);
		bool pushSessionMessage(const std::string& sessionId, const std::string& message);
		void closeAllSessions();

		/* Binds the first free port at or above preferredPort, on a fresh server
		   object per attempt, and returns it with the port it landed on. Null
		   means no port in the scanned range could be bound. */
		std::unique_ptr<httplib::Server> bindFirstFreeServer(uint16_t preferredPort, uint16_t& boundPort);

	   private:
		McpContext* context;
		Bridge* bridge;
		spdlog::logger* logger;

		ToolRegistry registry;
		Protocol protocol;

		std::unique_ptr<httplib::Server> httpServer;
		std::thread listenThread;

		std::atomic<bool> running{false};
		std::atomic<uint16_t> port{0};
		std::string lastError;

		std::mutex sessionsMtx;
		std::map<std::string, std::shared_ptr<Session>> sessions;
		std::atomic<uint64_t> sessionCounter{0};
	};
}  // namespace mcp

#endif
