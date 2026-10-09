#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socketType = SOCKET;
static constexpr socketType invalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socketType = int;
static constexpr socketType invalidSocket = -1;
#endif

#include "GdbServerDebugProbe.hpp"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/spdlog.h"

/*
 * The probe that reads through a GDB server.
 *
 * What makes it worth having is that it knows nothing about the target: the
 * addresses come from the symbol file, and the probe only has to speak the
 * protocol. So the tests below are about the protocol - a packet out with its
 * checksum, the acknowledgement that comes back, a memory read answered with
 * bytes - and about what happens when the server answers something else.
 *
 * The server here is a real one: it listens on a port the system picks and
 * speaks the same packets. It serves a single contiguous span of memory, which
 * is what a block read walks across.
 */

namespace
{
	std::string checksumOf(const std::string& body)
	{
		uint32_t sum = 0;

		for (const char c : body)
			sum += static_cast<uint8_t>(c);

		char text[4]{};
		std::snprintf(text, sizeof(text), "%02x", sum & 0xFF);

		return text;
	}

	class MockGdbServer
	{
	   public:
		explicit MockGdbServer(uint32_t packetSizeOffered = 256)
		{
#ifdef _WIN32
			WSADATA data{};
			WSAStartup(MAKEWORD(2, 2), &data);
#endif
			packetSize = packetSizeOffered;

			listener = ::socket(AF_INET, SOCK_STREAM, 0);

			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = 0;

			::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
			::listen(listener, 1);

			sockaddr_in bound{};
			socklen_t length = sizeof(bound);
			::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &length);
			boundPort = ntohs(bound.sin_port);

			worker = std::thread([this] { serveOneClient(); });
		}

		~MockGdbServer()
		{
			stopping = true;

			if (client != invalidSocket)
			{
#ifdef _WIN32
				::shutdown(client, SD_BOTH);
#else
				::shutdown(client, SHUT_RDWR);
#endif
			}

			worker.join();
		}

		uint16_t port() const { return boundPort; }

		/* One contiguous span, so that a block read walking past the first
		   chunk still lands inside it. */
		void serve(uint32_t address, const std::vector<uint8_t>& bytes)
		{
			std::lock_guard<std::mutex> lock(state);
			base = address;
			image = bytes;
		}

		std::vector<std::string> commands() const
		{
			std::lock_guard<std::mutex> lock(state);
			return seen;
		}

		bool detached() const
		{
			std::lock_guard<std::mutex> lock(state);
			return sawDetach;
		}

		/* A server that refuses every read, for the test that wants one. */
		std::atomic<bool> refuseReads{false};

		/* A server that talks over its own answers, the way one printing to a
		   console does. */
		std::atomic<bool> chatter{false};

	   private:
		void serveOneClient()
		{
			/* Waited for with a deadline so that a test which never connects
			   can still shut this down. */
			while (!stopping)
			{
				fd_set readable{};
				FD_ZERO(&readable);
				FD_SET(listener, &readable);

				timeval timeout{};
				timeout.tv_sec = 0;
				timeout.tv_usec = 50000;

#ifdef _WIN32
				const int32_t ready = select(0, &readable, nullptr, nullptr, &timeout);
#else
				const int32_t ready = select(listener + 1, &readable, nullptr, nullptr, &timeout);
#endif

				if (ready <= 0)
					continue;

				sockaddr_in from{};
				socklen_t fromLength = sizeof(from);
				client = ::accept(listener, reinterpret_cast<sockaddr*>(&from), &fromLength);

				if (client != invalidSocket)
					break;
			}

			if (client == invalidSocket)
				return;

			std::string body;

			while (readPacket(body))
			{
				{
					std::lock_guard<std::mutex> lock(state);
					seen.push_back(body);

					if (body == "D")
						sawDetach = true;
				}

				if (body == "qSupported")
					reply("PacketSize=" + toHex(packetSize) + ";qXfer:memory-map:read-");

				else if (!body.empty() && body[0] == 'm')
					answerRead(body);

				else if (!body.empty() && body[0] == 'M')
					reply("OK");

				else
					reply("");
			}
		}

		void answerRead(const std::string& body)
		{
			const size_t comma = body.find(',');

			if (comma == std::string::npos)
			{
				reply("E01");
				return;
			}

			const uint32_t address = static_cast<uint32_t>(std::strtoul(body.c_str() + 1, nullptr, 16));
			const uint32_t length = static_cast<uint32_t>(std::strtoul(body.c_str() + comma + 1, nullptr, 16));

			if (refuseReads)
			{
				reply("E01");
				return;
			}

			std::lock_guard<std::mutex> lock(state);
			const uint64_t offset = static_cast<uint64_t>(address) - base;

			if (address < base || offset + length > image.size())
			{
				reply("E01");
				return;
			}

			std::string text;
			text.reserve(length * 2);

			for (uint32_t i = 0; i < length; i++)
				text += toByte(image[static_cast<size_t>(offset) + i]);

			reply(text);
		}

		void reply(const std::string& body)
		{
			/* A server is allowed to interleave its own console output, which
			   is an 'O' packet carrying hex. It is not an answer to anything and
			   the probe has to step over it. */
			if (chatter)
				rawFrame("O" + hexOfText("working"));

			rawFrame(body);
		}

		void rawFrame(const std::string& body)
		{
			const std::string frame = "$" + body + "#" + checksumOf(body);
			::send(client, frame.data(), static_cast<int>(frame.size()), 0);
		}

		static std::string hexOfText(const std::string& text)
		{
			std::string out;

			for (const char c : text)
				out += toByte(static_cast<uint8_t>(c));

			return out;
		}

		bool readPacket(std::string& body)
		{
			char c = 0;

			while (true)
			{
				if (!receive(&c, 1))
					return false;

				if (c == '$')
					break;
			}

			body.clear();

			while (true)
			{
				if (!receive(&c, 1))
					return false;

				if (c == '#')
					break;

				body.push_back(c);
			}

			char checksum[3]{};
			if (!receive(checksum, 2))
				return false;

			/* Every packet is acknowledged before its answer is sent. */
			::send(client, "+", 1, 0);

			return true;
		}

		bool receive(char* data, size_t size)
		{
			size_t taken = 0;

			while (taken < size)
			{
				const int32_t read = static_cast<int32_t>(recv(client, data + taken, static_cast<int>(size - taken), 0));

				if (read <= 0)
					return false;

				taken += static_cast<size_t>(read);
			}

			return true;
		}

		static std::string toHex(uint32_t value)
		{
			char text[16]{};
			std::snprintf(text, sizeof(text), "%x", value);
			return text;
		}

		static std::string toByte(uint8_t value)
		{
			char text[4]{};
			std::snprintf(text, sizeof(text), "%02x", value);
			return text;
		}

		socketType listener = invalidSocket;
		socketType client = invalidSocket;

		uint16_t boundPort = 0;
		uint32_t packetSize = 256;

		std::thread worker;
		mutable std::mutex state;
		std::atomic<bool> stopping{false};

		uint32_t base = 0;
		std::vector<uint8_t> image;
		std::vector<std::string> seen;
		bool sawDetach = false;
	};
}

class GdbServerDebugProbeTest : public ::testing::Test
{
   protected:
	void SetUp() override
	{
		logger = std::make_shared<spdlog::logger>("gdb-server-probe-test", std::make_shared<spdlog::sinks::null_sink_mt>());
	}

	IDebugProbe::DebugProbeSettings settingsFor(const MockGdbServer& server) const
	{
		IDebugProbe::DebugProbeSettings settings;
		settings.debugProbe = IDebugProbe::Probe::GdbServer;
		settings.ipAddress = "127.0.0.1";
		settings.port = server.port();

		return settings;
	}

	std::shared_ptr<spdlog::logger> logger;
};

TEST_F(GdbServerDebugProbeTest, aReadComesBackAsTheBytesTheServerServes)
{
	MockGdbServer server;
	server.serve(0x20000000, {0x11, 0x22, 0x33, 0x44});

	GdbServerDebugProbe probe(logger.get());

	std::vector<std::pair<uint32_t, uint8_t>> addresses;
	ASSERT_TRUE(probe.startAcqusition(settingsFor(server), addresses, 100));

	uint8_t buffer[4]{};
	ASSERT_TRUE(probe.readMemory(0x20000000, buffer, sizeof(buffer)));

	EXPECT_EQ(buffer[0], 0x11);
	EXPECT_EQ(buffer[1], 0x22);
	EXPECT_EQ(buffer[2], 0x33);
	EXPECT_EQ(buffer[3], 0x44);

	probe.stopAcqusition();
}

TEST_F(GdbServerDebugProbeTest, aWriteGoesOutAsTheProtocolSays)
{
	MockGdbServer server;

	GdbServerDebugProbe probe(logger.get());

	std::vector<std::pair<uint32_t, uint8_t>> addresses;
	ASSERT_TRUE(probe.startAcqusition(settingsFor(server), addresses, 100));

	uint8_t value[4]{0xDE, 0xAD, 0xBE, 0xEF};
	const bool written = probe.writeMemory(0x20000004, value, sizeof(value));

	const std::vector<std::string> commands = server.commands();

	ASSERT_TRUE(written) << "error: " << probe.getLastErrorMsg();
	ASSERT_FALSE(commands.empty());

	EXPECT_EQ(commands.back(), "M20000004,4:deadbeef");

	probe.stopAcqusition();
}

TEST_F(GdbServerDebugProbeTest, theConsoleOutputAServerInterleavesIsSteppedOver)
{
	/* A server printing to its console sends those lines as 'O' packets, and
	   they can land between a request and its answer. The answer to a write is
	   "OK", which also begins with an 'O', so the two have to be told apart by
	   what follows the letter rather than by the letter alone. */
	MockGdbServer server;
	server.chatter = true;

	std::vector<uint8_t> bytes{0x01, 0x02, 0x03, 0x04};
	server.serve(0x20000000, bytes);

	GdbServerDebugProbe probe(logger.get());

	std::vector<std::pair<uint32_t, uint8_t>> addresses;
	ASSERT_TRUE(probe.startAcqusition(settingsFor(server), addresses, 100));

	uint8_t value[4]{0xDE, 0xAD, 0xBE, 0xEF};

	EXPECT_TRUE(probe.writeMemory(0x20000004, value, sizeof(value))) << probe.getLastErrorMsg();

	uint8_t buffer[4]{};
	EXPECT_TRUE(probe.readMemory(0x20000000, buffer, sizeof(buffer))) << probe.getLastErrorMsg();
	EXPECT_EQ(buffer[0], 0x01);
	EXPECT_EQ(buffer[3], 0x04);

	probe.stopAcqusition();
}

TEST_F(GdbServerDebugProbeTest, aRefusedReadIsReportedRatherThanSilentlyZero)
{
	MockGdbServer server;
	server.refuseReads = true;

	GdbServerDebugProbe probe(logger.get());

	std::vector<std::pair<uint32_t, uint8_t>> addresses;
	ASSERT_TRUE(probe.startAcqusition(settingsFor(server), addresses, 100));

	uint8_t buffer[4]{};

	EXPECT_FALSE(probe.readMemory(0x20000000, buffer, sizeof(buffer)));
	EXPECT_NE(probe.getLastErrorMsg().find("E01"), std::string::npos) << probe.getLastErrorMsg();

	probe.stopAcqusition();
}

TEST_F(GdbServerDebugProbeTest, aBlockIsReadInTheSizeTheServerOffers)
{
	/* The recorder buffer is downloaded through readBlock, so the range is
	   walked in what one packet can carry rather than one word at a time. This
	   server says it takes 256 bytes in a packet, so a quarter of that comes
	   back per answer and a kilobyte in a handful of them. */
	MockGdbServer server(256);

	std::vector<uint8_t> bytes(1024);

	for (size_t i = 0; i < bytes.size(); i++)
		bytes[i] = static_cast<uint8_t>(i);

	server.serve(0x20001000, bytes);

	GdbServerDebugProbe probe(logger.get());

	std::vector<std::pair<uint32_t, uint8_t>> addresses;
	ASSERT_TRUE(probe.startAcqusition(settingsFor(server), addresses, 100));

	uint8_t buffer[1024]{};
	ASSERT_TRUE(probe.readBlock(0x20001000, buffer, sizeof(buffer)));

	EXPECT_EQ(std::memcmp(buffer, bytes.data(), bytes.size()), 0);
	EXPECT_LT(server.commands().size(), 1024u / 4);

	probe.stopAcqusition();
}

TEST_F(GdbServerDebugProbeTest, theSizeTheServerOffersIsWhatTheProbeUses)
{
	/* A server that says it takes a long packet should be read in long
	   packets. The same kilobyte then costs far fewer round trips than the
	   default would. */
	MockGdbServer server(4096);

	std::vector<uint8_t> bytes(1024, 0x5A);
	server.serve(0x20002000, bytes);

	GdbServerDebugProbe probe(logger.get());

	std::vector<std::pair<uint32_t, uint8_t>> addresses;
	ASSERT_TRUE(probe.startAcqusition(settingsFor(server), addresses, 100));

	uint8_t buffer[1024]{};
	ASSERT_TRUE(probe.readBlock(0x20002000, buffer, sizeof(buffer)));

	EXPECT_EQ(std::memcmp(buffer, bytes.data(), bytes.size()), 0);

	probe.stopAcqusition();
}

TEST_F(GdbServerDebugProbeTest, stoppingTellsTheServerTheClientIsLeaving)
{
	MockGdbServer server;

	GdbServerDebugProbe probe(logger.get());

	std::vector<std::pair<uint32_t, uint8_t>> addresses;
	ASSERT_TRUE(probe.startAcqusition(settingsFor(server), addresses, 100));

	probe.stopAcqusition();

	EXPECT_TRUE(server.detached());
	EXPECT_FALSE(probe.isValid());
}

TEST_F(GdbServerDebugProbeTest, aServerThatIsNotThereIsReported)
{
	GdbServerDebugProbe probe(logger.get());

	IDebugProbe::DebugProbeSettings settings;
	settings.debugProbe = IDebugProbe::Probe::GdbServer;
	settings.ipAddress = "127.0.0.1";
	settings.port = 1;

	std::vector<std::pair<uint32_t, uint8_t>> addresses;

	EXPECT_FALSE(probe.startAcqusition(settings, addresses, 100));
	EXPECT_FALSE(probe.getLastErrorMsg().empty());
}

TEST_F(GdbServerDebugProbeTest, anAddressIsRequired)
{
	GdbServerDebugProbe probe(logger.get());

	IDebugProbe::DebugProbeSettings settings;
	settings.debugProbe = IDebugProbe::Probe::GdbServer;
	settings.ipAddress = "";

	std::vector<std::pair<uint32_t, uint8_t>> addresses;

	EXPECT_FALSE(probe.startAcqusition(settings, addresses, 100));
	EXPECT_NE(probe.getLastErrorMsg().find("address"), std::string::npos);
}
