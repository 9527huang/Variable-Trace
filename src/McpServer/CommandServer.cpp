#if defined(_WIN32)
/* winsock2.h has to be reached before windows.h, which spdlog pulls in through
   its own headers. Fixing the order here is what the warning asks for. */
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

using socketHandle = SOCKET;

static constexpr socketHandle invalidSocket = INVALID_SOCKET;

static int closeSocket(socketHandle handle)
{
	return closesocket(handle);
}

static void shutdownSocket(socketHandle handle)
{
	shutdown(handle, SD_BOTH);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

using socketHandle = int;

static constexpr socketHandle invalidSocket = -1;

static int closeSocket(socketHandle handle)
{
	return close(handle);
}

static void shutdownSocket(socketHandle handle)
{
	shutdown(handle, SHUT_RDWR);
}
#endif

#include "CommandServer.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

#include "spdlog/spdlog.h"

#include "McpProtocol.hpp"
#include "McpServer.hpp"

/*
 * The request and the reply.
 *
 * A request is one line holding a JSON object with "tool" and, optionally,
 * "arguments". A reply is one line holding a JSON object with "ok" and either
 * "result" or "error". One line in, one line out keeps the client side small
 * enough to write in a scripting language or a PLC program.
 *
 * Both ends speak UTF-8. The writer escapes what it has to, so a newline can
 * never appear inside a string and the reader can stop at the first one it
 * sees.
 */
namespace
{
	/* A client that never sends a newline must not be able to grow the read
	   buffer without end. */
	constexpr size_t maximumRequestLength = 64 * 1024;

	/* How long a read blocks before the loop looks at the stop flag again. */
	constexpr int readTimeoutSeconds = 1;

	static bool sendAll(socketHandle handle, const std::string& data)
	{
		size_t sent = 0;

		while (sent < data.size())
		{
			const int chunk = static_cast<int>(std::min<size_t>(data.size() - sent, 64 * 1024));

#if defined(_WIN32)
			const int result = ::send(handle, data.data() + sent, chunk, 0);
#else
			const ssize_t result = ::send(handle, data.data() + sent, chunk, 0);
#endif

			if (result <= 0)
				return false;

			sent += static_cast<size_t>(result);
		}

		return true;
	}
}  // namespace

CommandServer::CommandServer(mcp::Server* mcpServer, spdlog::logger* logger) : mcpServer(mcpServer), logger(logger)
{
#if defined(_WIN32)
	/* Winsock needs starting once per process. Doing it here keeps the call next
	   to the code that depends on it, and repeating it is harmless. */
	WSADATA data;

	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		logger->error("Winsock could not be started, the command channel is unavailable");
#endif
}

CommandServer::~CommandServer()
{
	stop();
}

bool CommandServer::start(bool enabled, uint16_t preferredPort)
{
	if (!enabled)
	{
		lastError = "The command channel is disabled in the preferences.";
		return false;
	}

	if (running.load())
		return true;

	/* Zero means "pick a port", which is what the MCP listener does as well. A
	   scan starting at zero would try the privileged ports in turn and fail on
	   every one of them. */
	if (preferredPort == 0)
		preferredPort = defaultPort;

	/* The address is filled in once and reused for every candidate. */
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	for (uint32_t offset = 0; offset < portScanRange; offset++)
	{
		const uint32_t candidate = static_cast<uint32_t>(preferredPort) + offset;

		/* The counter is wide enough that a base near the top of the range does
		   not wrap around to the privileged ports. */
		if (candidate > 65535)
			break;

		const socketHandle handle = ::socket(AF_INET, SOCK_STREAM, 0);

		if (handle == invalidSocket)
		{
			lastError = "A listening socket could not be created.";
			logger->error("{}", lastError);
			return false;
		}

		/* Without this a restart in the same second fails on a socket that is
		   still in the shutdown path. */
		int reuse = 1;
		setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

		address.sin_port = htons(static_cast<uint16_t>(candidate));

		if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
		{
			closeSocket(handle);
			continue;
		}

		if (::listen(handle, 4) != 0)
		{
			closeSocket(handle);
			continue;
		}

		listenSocket = static_cast<intptr_t>(handle);
		port.store(static_cast<uint16_t>(candidate));
		stopping.store(false);
		running.store(true);

		acceptThread = std::thread(&CommandServer::acceptLoop, this);

		logger->info("Command channel listening on tcp://127.0.0.1:{}", candidate);

		return true;
	}

	lastError = "No free port for the command channel between " + std::to_string(preferredPort) + " and " +
				std::to_string(preferredPort + portScanRange - 1) + ".";
	logger->error("{}", lastError);

	return false;
}

void CommandServer::stop()
{
	/* -1 is the "nothing is listening" marker this class uses on every platform,
	   rather than INVALID_SOCKET, so that the member can stay a plain integer. */
	if (listenSocket == -1)
		return;

	stopping.store(true);

	/* Closing the listening socket is what releases accept(). */
	const socketHandle handle = static_cast<socketHandle>(listenSocket);
	listenSocket = -1;

	closeSocket(handle);

	if (acceptThread.joinable())
		acceptThread.join();

	if (running.exchange(false))
		logger->info("Command channel stopped");

	port.store(0);
}

void CommandServer::restart(bool enabled, uint16_t preferredPort)
{
	stop();
	start(enabled, preferredPort);
}

bool CommandServer::isRunning() const
{
	return running.load();
}

uint16_t CommandServer::getPort() const
{
	return port.load();
}

std::string CommandServer::getLastError() const
{
	return lastError;
}

std::string CommandServer::getEndpoint() const
{
	if (!running.load())
		return "";

	return "tcp://127.0.0.1:" + std::to_string(port.load());
}

size_t CommandServer::getRequestCount() const
{
	return static_cast<size_t>(requestCount.load());
}

void CommandServer::acceptLoop()
{
	while (!stopping.load())
	{
		const socketHandle listener = static_cast<socketHandle>(listenSocket);

		/* The sentinel differs by platform - INVALID_SOCKET on Windows is an
		   unsigned value - so it is compared against rather than tested for
		   sign. */
		if (listener == invalidSocket)
			break;

		sockaddr_in peer{};

#if defined(_WIN32)
		int peerLength = sizeof(peer);
#else
		socklen_t peerLength = sizeof(peer);
#endif

		const socketHandle client = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLength);

		if (client == invalidSocket)
		{
			if (stopping.load())
				break;

			/* A refused connection is not a reason to give up the listener. */
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}

		/* Served inline rather than on a thread of its own. A command that
		   touches the model already waits for the GUI thread, so a second
		   connection would only queue behind the first one anyway, and a
		   connection that is not read from would hold the channel open. */
		serveConnection(static_cast<intptr_t>(client));

		shutdownSocket(client);
		closeSocket(client);
	}
}

void CommandServer::serveConnection(intptr_t rawClient)
{
	const socketHandle client = static_cast<socketHandle>(rawClient);

	/* A read that blocks forever would keep stop() waiting on the join, so the
	   socket is woken up on its own after a second and the loop rechecks. */
#if defined(_WIN32)
	DWORD timeout = readTimeoutSeconds * 1000;
	setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
	timeval timeout{};
	timeout.tv_sec = readTimeoutSeconds;
	setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

	std::string pending;

	while (!stopping.load())
	{
		char buffer[4096];

#if defined(_WIN32)
		const int received = ::recv(client, buffer, sizeof(buffer), 0);
#else
		const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
#endif

		if (received == 0)
			break;

		if (received < 0)
		{
#if defined(_WIN32)
			const int code = WSAGetLastError();

			if (code == WSAETIMEDOUT)
				continue;
#else
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
#endif

			break;
		}

		pending.append(buffer, static_cast<size_t>(received));

		if (pending.size() > maximumRequestLength)
		{
			const std::string reply = mcp::Json({{"ok", false},
												 {"error", "The request is longer than the 64 kB limit."}})
										  .dump() +
									  "\n";
			sendAll(client, reply);
			break;
		}

		for (;;)
		{
			const size_t newline = pending.find('\n');

			if (newline == std::string::npos)
				break;

			std::string line = pending.substr(0, newline);
			pending.erase(0, newline + 1);

			/* A trailing carriage return is what a terminal client sends. */
			if (!line.empty() && line.back() == '\r')
				line.pop_back();

			if (line.empty())
				continue;

			if (!sendAll(client, handleCommand(line)))
				return;
		}
	}
}

std::string CommandServer::handleCommand(const std::string& line)
{
	mcp::Json reply;
	mcp::Json request;

	try
	{
		request = mcp::Json::parse(line);
	}
	catch (const std::exception& error)
	{
		reply = {{"ok", false}, {"error", std::string("The request is not valid JSON: ") + error.what()}};
		return reply.dump() + "\n";
	}

	if (!request.is_object())
	{
		reply = {{"ok", false}, {"error", "The request must be a JSON object."}};
		return reply.dump() + "\n";
	}

	if (!request.contains("tool") || !request.at("tool").is_string())
	{
		reply = {{"ok", false}, {"error", "The request needs a 'tool' field naming the tool to call."}};
		return reply.dump() + "\n";
	}

	const std::string tool = request.at("tool").get<std::string>();

	/* An omitted field means the tool takes no arguments, which is what the HTTP
	   flavour sends as well. */
	mcp::Json arguments = mcp::Json::object();

	if (request.contains("arguments") && !request.at("arguments").is_null())
	{
		if (!request.at("arguments").is_object())
		{
			reply = {{"ok", false}, {"error", "The 'arguments' field must be a JSON object."}};
			return reply.dump() + "\n";
		}

		arguments = request.at("arguments");
	}

	requestCount.fetch_add(1);

	try
	{
		const mcp::Json envelope = mcpServer->callTool(tool, arguments);
		const bool failed = envelope.value("isError", false);

		/* The envelope wraps the payload as text, because that is what an MCP
		   client reads. A caller here wants the value itself, so the text is
		   parsed back out and, when that fails, passed on as it stands. */
		std::string text;

		if (envelope.contains("content") && envelope.at("content").is_array() && !envelope.at("content").empty())
			text = envelope.at("content").at(0).value("text", std::string());

		if (failed)
		{
			reply = {{"ok", false}, {"error", text}};
			return reply.dump() + "\n";
		}

		mcp::Json payload;

		try
		{
			payload = mcp::Json::parse(text);
		}
		catch (const std::exception&)
		{
			payload = text;
		}

		reply = {{"ok", true}, {"result", payload}};
	}
	catch (const mcp::JsonRpcError& error)
	{
		reply = {{"ok", false}, {"error", error.what()}, {"code", error.getCode()}};
	}
	catch (const std::exception& error)
	{
		reply = {{"ok", false}, {"error", std::string("Internal error: ") + error.what()}};
	}
	catch (...)
	{
		reply = {{"ok", false}, {"error", "Internal error."}};
	}

	return reply.dump() + "\n";
}
