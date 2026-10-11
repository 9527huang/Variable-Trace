#ifndef _COMMANDSERVER_HPP
#define _COMMANDSERVER_HPP

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "McpTypes.hpp"

namespace spdlog
{
	class logger;
}

namespace mcp
{
	class Server;
}

/*
 * A line based command channel over TCP, for callers that are not able to
 * speak HTTP.
 *
 * A PLC, a bench script or a terminal can open a socket, send one request per
 * line and read one reply per line. The request names a tool and carries its
 * arguments as JSON; the reply carries the result, or the reason it failed.
 * The work itself is handed to the MCP server, so the same tools, the same
 * argument checking and the same hand over to the GUI thread apply here. This
 * class adds a transport, not a second way to touch the model.
 *
 * The listener is bound to the loopback interface. The channel can write to
 * the target, so it has to stay off the network.
 */
class CommandServer
{
   public:
	static constexpr uint16_t defaultPort = 7808;

	/* How far the search walks forward when the preferred port is taken. */
	static constexpr uint16_t portScanRange = 20;

	CommandServer(mcp::Server* mcpServer, spdlog::logger* logger);
	~CommandServer();

	CommandServer(const CommandServer&) = delete;
	CommandServer& operator=(const CommandServer&) = delete;

	/* Binds the first free port at or above preferredPort and starts accepting.
	   False means the channel is off; the reason is then available through
	   getLastError(). */
	bool start(bool enabled, uint16_t preferredPort);

	void stop();

	/* Rebuilt when the port setting changes, the same way the MCP listener is. */
	void restart(bool enabled, uint16_t preferredPort);

	bool isRunning() const;
	uint16_t getPort() const;
	std::string getLastError() const;

	/* The address a client is configured with. Empty while not running. */
	std::string getEndpoint() const;

	size_t getRequestCount() const;

	/* Handles one request line and returns the reply line, newline included.
	 *
	 * Public without sockets so that the tests can exercise the request and
	 * reply shapes without a listening port. */
	std::string handleCommand(const std::string& line);

   private:
	void acceptLoop();
	void serveConnection(intptr_t clientSocket);

   private:
	mcp::Server* mcpServer;
	spdlog::logger* logger;

	intptr_t listenSocket = -1;
	std::thread acceptThread;

	std::atomic<bool> running{false};
	std::atomic<bool> stopping{false};
	std::atomic<uint16_t> port{0};
	std::atomic<uint64_t> requestCount{0};
	std::string lastError;
};

#endif
