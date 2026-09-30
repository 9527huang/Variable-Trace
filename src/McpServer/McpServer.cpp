#if defined(_WIN32)
/* httplib includes winsock2.h, and by the time this file reaches it spdlog has
   already pulled in windows.h, which makes winsock2 complain. Fixing the order
   here is what the warning asks for. */
#include <winsock2.h>

#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "McpServer.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <vector>

#include "McpTools.hpp"
#include "httplib.h"

namespace
{
	constexpr const char* jsonContentType = "application/json";
	constexpr const char* sseContentType = "text/event-stream";

	/* How long a server sent events stream waits before it sends a comment to
	   keep the connection alive. */
	constexpr std::chrono::seconds sseKeepAlive{15};

	constexpr size_t maxOpenSessions = 16;

#if defined(_WIN32)
	constexpr const char* platformName = "windows";
#elif defined(__APPLE__)
	constexpr const char* platformName = "macos";
#elif defined(__linux__)
	constexpr const char* platformName = "linux";
#else
	constexpr const char* platformName = "unknown";
#endif

	/* Winsock and POSIX disagree about the type of a socket and about how it is
	   closed, so the two are wrapped once here. */
#if defined(_WIN32)
	using RawSocket = SOCKET;
	constexpr RawSocket invalidRawSocket = INVALID_SOCKET;

	void closeRawSocket(RawSocket handle)
	{
		::closesocket(handle);
	}

	bool lastConnectIsPending()
	{
		return WSAGetLastError() == WSAEWOULDBLOCK;
	}

	bool makeNonBlocking(RawSocket handle)
	{
		u_long enabled = 1;
		return ::ioctlsocket(handle, FIONBIO, &enabled) == 0;
	}
#else
	using RawSocket = int;
	constexpr RawSocket invalidRawSocket = -1;

	void closeRawSocket(RawSocket handle)
	{
		::close(handle);
	}

	bool lastConnectIsPending()
	{
		return errno == EINPROGRESS;
	}

	bool makeNonBlocking(RawSocket handle)
	{
		return ::fcntl(handle, F_SETFL, O_NONBLOCK) == 0;
	}
#endif

	/*
	 * Waits for a non blocking connect to finish. True means it got through, so
	 * something is listening.
	 */
	bool connectCompletes(RawSocket handle, int milliseconds)
	{
		fd_set writable;
		FD_ZERO(&writable);
		FD_SET(handle, &writable);

		timeval timeout{};
		timeout.tv_sec = milliseconds / 1000;
		timeout.tv_usec = (milliseconds % 1000) * 1000;

#if defined(_WIN32)
		const int ready = ::select(0, nullptr, &writable, nullptr, &timeout);
#else
		const int ready = ::select(handle + 1, nullptr, &writable, nullptr, &timeout);
#endif

		if (ready <= 0)
			return false;

		int pending = 0;
		socklen_t length = sizeof(pending);

		if (::getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pending), &length) != 0)
			return false;

		return pending == 0;
	}

	/*
	 * True when something is listening on the port.
	 *
	 * The search for a free port asks this instead of relying on a bind.
	 * httplib sets SO_REUSEADDR, and on Windows that lets a second server bind a
	 * port a first server is already listening on, so a bind that succeeds
	 * proves nothing and two instances end up sharing one port. An exclusive
	 * bind would notice the other server, but a port that has just been released
	 * refuses an exclusive bind for a while, so restarting would move the server
	 * off the port that was configured for it. Connecting asks the question
	 * directly and is affected by neither.
	 */
	bool isPortOccupied(uint16_t port)
	{
		const RawSocket probe = ::socket(AF_INET, SOCK_STREAM, 0);

		/* Without a socket the port cannot be checked, so it is treated as taken
		   and the search moves on rather than risking a shared port. */
		if (probe == invalidRawSocket)
			return true;

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		bool occupied = false;

		if (makeNonBlocking(probe))
		{
			if (::connect(probe, reinterpret_cast<sockaddr*>(&address), static_cast<socklen_t>(sizeof(address))) == 0)
				occupied = true;
			else if (lastConnectIsPending())
			{
				/* A loopback refusal arrives at once, so this only matters when
				   something swallows the attempt, and then it must not hold the
				   search up. */
				occupied = connectCompletes(probe, 200);
			}
		}

		closeRawSocket(probe);

		return occupied;
	}

	/*
	 * Identity of the running instance. The list of subsystems is part of it
	 * because the tools of a missing subsystem answer with a message that points
	 * here, so a client needs to be able to find out what is missing without
	 * calling every tool once.
	 */
	mcp::Json buildInstanceInfo(McpContext& context, size_t toolCount)
	{
		std::string version = "unknown";

		if (context.getApplicationVersion)
			version = context.getApplicationVersion();

		uint16_t port = 0;

		if (context.getServerPort)
			port = context.getServerPort();

		/* Reported from what is actually wired rather than written down, so the
		   answer cannot disagree with the tools that use the subsystem. */
		mcp::Json result = mcp::Json::object({{"app", mcp::serverName},
											  {"version", version},
											  {"port", port},
											  {"endpoint", port == 0 ? "" : ("http://127.0.0.1:" + std::to_string(port) + "/mcp")},
											  {"tools", toolCount},
											  {"platform", platformName},
											  {"subsystems", mcp::Json::object({{"recorder", context.recorderHandler != nullptr}, {"flashing", false}})}});

		if (context.getProjectPath)
		{
			const std::string projectPath = context.getProjectPath();

			if (!projectPath.empty())
				result["project_path"] = projectPath;
		}

		if (context.getElfPath)
		{
			const std::string elfPath = context.getElfPath();

			if (!elfPath.empty())
				result["elf_path"] = elfPath;
		}

		if (context.getApiWritesEnabled)
			result["api_writes_enabled"] = context.getApiWritesEnabled();

		return result;
	}
}  // namespace

/*
 * One server sent events client. The provider of the open stream blocks on the
 * condition variable until the protocol handler queues an answer for it.
 */
struct mcp::Server::Session
{
	std::mutex mtx;
	std::condition_variable cv;
	std::deque<std::string> messages;

	std::string messagesPath;
	bool endpointSent = false;
	bool closed = false;

	/* True when a message was taken out. False means the wait ended on a timeout
	   or because the session was closed; isClosed() tells the two apart. */
	bool takeMessage(std::string& message, std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mtx);

		if (messages.empty() && !closed)
			cv.wait_for(lock, timeout, [this] { return !messages.empty() || closed; });

		if (messages.empty())
			return false;

		message = messages.front();
		messages.pop_front();
		return true;
	}

	bool isClosed()
	{
		std::lock_guard<std::mutex> lock(mtx);
		return closed;
	}

	void close()
	{
		{
			std::lock_guard<std::mutex> lock(mtx);
			closed = true;
		}

		cv.notify_all();
	}
};

mcp::Server::Server(McpContext* context, Bridge* bridge, spdlog::logger* logger)
	: context(context),
	  bridge(bridge),
	  logger(logger),
	  protocol(&registry,
			   [this](const std::string& name, const Json& arguments)
			   { return callTool(name, arguments); },
			   [context]()
			   {
				   if (context != nullptr && context->getApplicationVersion)
					   return context->getApplicationVersion();

				   return std::string("unknown");
			   })
{
	registerTools();
}

mcp::Server::~Server()
{
	stop();
}

void mcp::Server::registerTools()
{
	ProjectTools::registerAll(registry);
	GroupTools::registerAll(registry);
	PlotTools::registerAll(registry);
	VariableTools::registerAll(registry);
	RecorderTools::registerAll(registry);
	AcquisitionTools::registerAll(registry);
	DataTools::registerAll(registry);
	FlashingTools::registerAll(registry);

	/* Identity of the server itself rather than of one of its domains. */
	registry.add({.name = "get_instance_info",
				  .description = "Returns identifying information about this MCUViewer instance: the application name and version, "
								 "the port and endpoint the API listens on, the number of registered tools, the platform, and which "
								 "optional subsystems this build provides.",
				  .inputSchema = objectSchema(Json::object({})),
				  .handler = [this](McpContext& context, const Json&)
				  { return buildInstanceInfo(context, registry.count()); }});

	logger->info("MCP server registered {} tools", registry.count());
}

std::unique_ptr<httplib::Server> mcp::Server::bindFirstFreeServer(uint16_t preferredPort, uint16_t& boundPort)
{
	/*
	 * A new object is built for every candidate rather than reusing one. httplib
	 * marks a server as decommissioned as soon as a bind fails, and every later
	 * call on that object returns immediately, so one object can only ever
	 * attempt a single port.
	 */
	if (preferredPort == 0)
		preferredPort = defaultPort;

	for (uint16_t offset = 0; offset <= portScanRange; offset++)
	{
		const uint32_t candidate = static_cast<uint32_t>(preferredPort) + offset;

		if (candidate > 65535)
			break;

		/* A port somebody is already listening on is skipped outright: bind()
		   alone would report success and leave the two instances sharing it. */
		if (isPortOccupied(static_cast<uint16_t>(candidate)))
			continue;

		auto server = std::make_unique<httplib::Server>();

		/* A server sent events stream occupies a worker for as long as the client
		   stays connected, so the pool is sized to leave room for concurrent calls. */
		server->new_task_queue = []
		{ return new httplib::ThreadPool(16); };

		registerRoutes(*server);

		if (server->bind_to_port("127.0.0.1", static_cast<int>(candidate)))
		{
			boundPort = static_cast<uint16_t>(candidate);
			return server;
		}
	}

	return nullptr;
}

bool mcp::Server::start(bool enabled, uint16_t preferredPort)
{
	stop();

	if (!enabled)
		return false;

	if (context == nullptr || bridge == nullptr)
	{
		lastError = "The server was created without an application context.";
		logger->error("MCP server: {}", lastError);
		return false;
	}

	uint16_t boundPort = 0;
	std::unique_ptr<httplib::Server> server = bindFirstFreeServer(preferredPort, boundPort);

	if (server == nullptr)
	{
		const uint32_t firstCandidate = preferredPort == 0 ? defaultPort : preferredPort;
		const uint32_t lastCandidate = firstCandidate + portScanRange;
		lastError = "No free port between " + std::to_string(firstCandidate) + " and " + std::to_string(lastCandidate) + ".";
		port = 0;
		logger->error("MCP server could not start: {}", lastError);
		return false;
	}

	port = boundPort;
	running = true;
	lastError.clear();
	httpServer = std::move(server);

	listenThread = std::thread([this]()
							   {
								   if (!httpServer->listen_after_bind())
								   {
									   running = false;
									   logger->error("MCP server stopped listening on port {}", port.load());
								   }
							   });

	/* A server object only closes its listener from inside stop() once its
	   listening thread has entered the accept loop. A stop() that arrives in the
	   gap before that does nothing at all, and the join in stop() would then wait
	   for a thread that is happily accepting connections. Waiting here closes
	   that gap. */
	httpServer->wait_until_ready();

	logger->info("MCP server listening on http://127.0.0.1:{}/mcp with {} tools", boundPort, registry.count());

	return true;
}

void mcp::Server::stop()
{
	/* The destructor calls this as well, so an owner that stopped the server
	   itself causes a second pass. That pass has nothing left to undo, and
	   skipping it keeps it from reaching into the bridge, which lives in the
	   owner and does not have to be alive any more by the time the server is
	   released. */
	if (!running && httpServer == nullptr && !listenThread.joinable())
		return;

	/* Stopping the server joins its connection threads, and such a thread can be
	   parked on the bridge waiting for the interface thread to run its tool call.
	   Those callers are released first, otherwise the join would wait for the
	   full request timeout. */
	if (bridge != nullptr)
		bridge->cancelWaitingTasks();

	if (httpServer != nullptr)
		httpServer->stop();

	/* The streams have to be released before the server is destroyed: a worker
	   blocked in the stream provider keeps the connection pool from shutting
	   down, and its provider waits on a condition variable that only this call
	   can signal. */
	closeAllSessions();

	if (listenThread.joinable())
		listenThread.join();

	httpServer.reset();

	running = false;
	port = 0;
}

void mcp::Server::restart(bool enabled, uint16_t preferredPort)
{
	if (!enabled)
	{
		stop();
		return;
	}

	if (start(true, preferredPort))
		logger->info("MCP server restarted on port {}", getPort());
}

bool mcp::Server::isRunning() const
{
	return running.load();
}

uint16_t mcp::Server::getPort() const
{
	return port.load();
}

std::string mcp::Server::getEndpoint() const
{
	if (!running.load())
		return "";

	return "http://127.0.0.1:" + std::to_string(port.load()) + "/mcp";
}

std::string mcp::Server::getLastError() const
{
	return lastError;
}

size_t mcp::Server::getToolCount() const
{
	return registry.count();
}

/* ------------------------------------------------------------------ routes */

void mcp::Server::registerRoutes(httplib::Server& server)
{
	/* Streamable HTTP. A single endpoint accepts every JSON-RPC message. */
	server.Post("/mcp", [this](const httplib::Request& request, httplib::Response& response)
				{
					Json message;

					try
					{
						message = Json::parse(request.body);
					}
					catch (const std::exception& error)
					{
						response.status = 400;
						response.set_content(Protocol::wrappedError(Json(), JsonRpcError::parseError, std::string("Malformed JSON: ") + error.what()).dump(),
											 jsonContentType);
						return;
					}

					const Json reply = handleMessage(message);

					/* A notification has no reply and the transport answers 202. */
					if (reply.is_null())
					{
						response.status = 202;
						return;
					}

					response.set_content(reply.dump(), jsonContentType);
				});

	/* Server initiated messages would arrive on a GET stream. This build has
	   none, so the method is refused explicitly rather than left hanging. */
	server.Get("/mcp", [](const httplib::Request&, httplib::Response& response)
			   {
				   response.status = 405;
				   response.set_header("Allow", "POST");
				   response.set_content("This server sends no unsolicited messages, use POST.", "text/plain");
			   });

	/* HTTP with server sent events, the transport older clients speak. */
	server.Get("/sse", [this](const httplib::Request&, httplib::Response& response)
			   {
				   const std::string sessionId = "s" + std::to_string(++sessionCounter);
				   auto session = openSession(sessionId);

				   if (session == nullptr)
				   {
					   response.status = 503;
					   response.set_content("Too many open event streams.", "text/plain");
					   return;
				   }

				   response.set_chunked_content_provider(
					   sseContentType,
					   [session](size_t, httplib::DataSink& sink) -> bool
					   {
						   std::string message;

						   {
							   std::lock_guard<std::mutex> lock(session->mtx);

							   if (!session->endpointSent)
							   {
								   session->endpointSent = true;
								   message = "event: endpoint\ndata: " + session->messagesPath + "\n\n";
							   }
						   }

						   if (message.empty() && !session->takeMessage(message, std::chrono::duration_cast<std::chrono::milliseconds>(sseKeepAlive)))
						   {
							   if (session->isClosed())
								   return false;

							   /* A comment is written rather than nothing at all:
							      the chunked writer treats an empty write as the end
							      of the response, and returning without writing
							      would spin this loop. */
							   static const std::string keepAlive = ": keep-alive\n\n";
							   return sink.write(keepAlive.data(), keepAlive.size());
						   }

						   if (message.empty())
							   return false;

						   return sink.write(message.data(), message.size());
					   },
					   [this, sessionId](bool)
					   {
						   std::lock_guard<std::mutex> lock(sessionsMtx);
						   sessions.erase(sessionId);
					   });
			   });

	server.Post("/messages", [this](const httplib::Request& request, httplib::Response& response)
				{
					const std::string sessionId = request.get_param_value("sessionId");
					Json message;

					try
					{
						message = Json::parse(request.body);
					}
					catch (const std::exception& error)
					{
						response.status = 400;
						response.set_content(std::string("Malformed JSON: ") + error.what(), "text/plain");
						return;
					}

					const Json reply = handleMessage(message);

					if (!reply.is_null())
					{
						std::string event = "event: message\ndata: ";
						event += reply.dump();
						event += "\n\n";

						if (!pushSessionMessage(sessionId, event))
						{
							response.status = 404;
							response.set_content("Unknown or closed session.", "text/plain");
							return;
						}
					}

					response.status = 202;
				});

	/* Convenience for a human checking whether the server is up. */
	server.Get("/", [this](const httplib::Request&, httplib::Response& response)
			   {
				   const Json status = Json::object({{"server", serverName},
													 {"port", port.load()},
													 {"endpoint", getEndpoint()},
													 {"tools", registry.count()},
													 {"transport", Json::array({"streamable-http /mcp", "sse /sse + /messages"})}});

				   response.set_content(status.dump(2), jsonContentType);
			   });
}

std::shared_ptr<mcp::Server::Session> mcp::Server::openSession(const std::string& sessionId)
{
	std::lock_guard<std::mutex> lock(sessionsMtx);

	if (sessions.size() >= maxOpenSessions)
		return nullptr;

	auto session = std::make_shared<Session>();
	session->messagesPath = "/messages?sessionId=" + sessionId;
	sessions.emplace(sessionId, session);

	logger->info("MCP event stream opened for session {}", sessionId);

	return session;
}

bool mcp::Server::pushSessionMessage(const std::string& sessionId, const std::string& message)
{
	std::lock_guard<std::mutex> lock(sessionsMtx);

	auto entry = sessions.find(sessionId);

	if (entry == sessions.end())
		return false;

	auto session = entry->second;

	{
		std::lock_guard<std::mutex> sessionLock(session->mtx);
		session->messages.push_back(message);
	}

	session->cv.notify_all();

	return true;
}

void mcp::Server::closeAllSessions()
{
	std::map<std::string, std::shared_ptr<Session>> local;

	{
		std::lock_guard<std::mutex> lock(sessionsMtx);
		local.swap(sessions);
	}

	for (auto& [id, session] : local)
		session->close();
}

/* -------------------------------------------------------------- protocol */

mcp::Json mcp::Server::handleMessage(const Json& message)
{
	return protocol.handleMessage(message);
}

mcp::Json mcp::Server::callTool(const std::string& name, const Json& arguments)
{
	const Tool* tool = registry.find(name);

	if (tool == nullptr)
		throw JsonRpcError(JsonRpcError::invalidParams, "Unknown tool '" + name + "'. Call tools/list for the available tools.");

	/* An argument name the schema does not declare is a typo, and an ignored
	   typo is worse than a refused one: the caller would be told that a setting
	   it never changed had been applied. */
	if (arguments.is_object())
	{
		const Json accepted = tool->inputSchema.value("properties", Json::object());
		std::vector<std::string> unknown;

		for (const auto& entry : arguments.items())
		{
			if (!accepted.contains(entry.key()))
				unknown.push_back("'" + entry.key() + "'");
		}

		if (!unknown.empty())
		{
			std::string message = "Unknown argument";

			if (unknown.size() > 1)
				message += "s";

			for (size_t index = 0; index < unknown.size(); index++)
				message += (index == 0 ? " " : ", ") + unknown[index];

			message += " for " + name + ". Accepted: ";

			if (accepted.empty())
				message += "none, this tool takes no arguments.";
			else
			{
				bool first = true;

				for (const auto& entry : accepted.items())
				{
					message += (first ? "" : ", ") + entry.key();
					first = false;
				}
			}

			throw JsonRpcError(JsonRpcError::invalidParams, message);
		}
	}

	Json payload;
	std::string failure;
	bool failed = false;

	/* A faulty tool must not be able to take the application down, so every
	   exception it raises is turned into a reported failure. */
	auto work = [&, tool]()
	{
		try
		{
			payload = tool->handler(*context, arguments);
		}
		catch (const ToolError& error)
		{
			failure = error.what();
			failed = true;
		}
		catch (const std::exception& error)
		{
			failure = std::string("Internal error: ") + error.what();
			failed = true;
		}
		catch (...)
		{
			failure = "Internal error.";
			failed = true;
		}
	};

	if (tool->onGuiThread)
	{
		/* A call whose task could not be run is reported as a tool failure rather
		   than as a protocol error: nothing about the request was wrong, the
		   application simply did not get to it. */
		if (!bridge->runOnGuiThread(work))
			return toolFailure("The application did not process the request. Check that no modal dialog is open, or that the server was not restarted, and try again.");
	}
	else
	{
		work();
	}

	if (failed)
		return toolFailure(failure);

	return toolSuccess(payload);
}
