#include "GdbServerDebugProbe.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
	/* The socket handle this platform uses, and the value that means there is
	   none. Windows spells it SOCKET/INVALID_SOCKET, everything else an int. */
	constexpr intptr_t noHandle = -1;

	constexpr int32_t connectTimeoutMs = 5000;
	constexpr int32_t ioTimeoutMs = 3000;

	/* The sum of the bytes of a packet body, as two hex digits. That is what
	   the protocol puts after the '#'. */
	std::string checksumOf(const std::string& body)
	{
		uint32_t sum = 0;

		for (const char c : body)
			sum += static_cast<uint8_t>(c);

		char text[4]{};
		std::snprintf(text, sizeof(text), "%02x", sum & 0xFF);

		return text;
	}

	uint8_t byteFromHex(const std::string& text, size_t offset)
	{
		return static_cast<uint8_t>(std::strtoul(text.substr(offset, 2).c_str(), nullptr, 16));
	}

	std::string hexOf(const uint8_t* data, size_t size)
	{
		static const char digits[] = "0123456789abcdef";

		std::string text;
		text.reserve(size * 2);

		for (size_t i = 0; i < size; i++)
		{
			text.push_back(digits[data[i] >> 4]);
			text.push_back(digits[data[i] & 0x0F]);
		}

		return text;
	}

	int32_t lastSocketError()
	{
#ifdef _WIN32
		return WSAGetLastError();
#else
		return errno;
#endif
	}
}

GdbServerDebugProbe::GdbServerDebugProbe(spdlog::logger* logger) : logger(logger)
{
#ifdef _WIN32
	/* The socket library has to be asked for once per process; doing it here
	   rather than somewhere central keeps this probe able to stand on its own,
	   and asking twice costs nothing. */
	WSADATA data{};
	WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

GdbServerDebugProbe::~GdbServerDebugProbe()
{
	closeLink();
}

bool GdbServerDebugProbe::isValid() const
{
	return isRunning;
}

std::string GdbServerDebugProbe::getLastErrorMsg() const
{
	return lastErrorMsg;
}

void GdbServerDebugProbe::closeLink()
{
	if (handle == noHandle)
		return;

#ifdef _WIN32
	closesocket(static_cast<SOCKET>(handle));
#else
	::close(static_cast<int>(handle));
#endif

	handle = noHandle;
	isRunning = false;
}

bool GdbServerDebugProbe::writeAll(const char* data, size_t size)
{
	size_t sent = 0;

	while (sent < size)
	{
		const int32_t written = static_cast<int32_t>(
#ifdef _WIN32
			send(static_cast<SOCKET>(handle), data + sent, static_cast<int>(size - sent), 0)
#else
			::send(static_cast<int>(handle), data + sent, size - sent, 0)
#endif
		);

		if (written <= 0)
		{
			lastErrorMsg = "The GDB server closed the connection while a packet was being sent (error " + std::to_string(lastSocketError()) + ")";
			return false;
		}

		sent += static_cast<size_t>(written);
	}

	return true;
}

bool GdbServerDebugProbe::readAll(char* data, size_t size)
{
	size_t taken = 0;

	while (taken < size)
	{
		const int32_t read = static_cast<int32_t>(
#ifdef _WIN32
			recv(static_cast<SOCKET>(handle), data + taken, static_cast<int>(size - taken), 0)
#else
			::recv(static_cast<int>(handle), data + taken, size - taken, 0)
#endif
		);

		if (read <= 0)
		{
			lastErrorMsg = "The GDB server did not answer (error " + std::to_string(lastSocketError()) + ")";
			return false;
		}

		taken += static_cast<size_t>(read);
	}

	return true;
}

bool GdbServerDebugProbe::sendPacket(const std::string& body)
{
	const std::string frame = "$" + body + "#" + checksumOf(body);

	if (!writeAll(frame.data(), frame.size()))
		return false;

	/* Every packet is acknowledged. A refusal means the server did not
	   understand it, and there is nothing useful to do but say so. */
	char answer = 0;

	if (!readAll(&answer, 1))
		return false;

	if (answer != '+')
	{
		lastErrorMsg = "The GDB server refused the packet '" + body + "'";
		return false;
	}

	return true;
}

bool GdbServerDebugProbe::receivePacket(std::string& reply)
{
	/* Anything before the '$' is either the acknowledgement of what was just
	   sent or a notification the server sends on its own; both are stepped
	   over. */
	while (true)
	{
		char start = 0;

		if (!readAll(&start, 1))
			return false;

		if (start != '$')
			continue;

		std::string body;

		while (true)
		{
			char c = 0;

			if (!readAll(&c, 1))
				return false;

			if (c == '#')
				break;

			body.push_back(c);
		}

		char checksumText[3]{};
		if (!readAll(checksumText, 2))
			return false;

		const bool intact = std::strtol(checksumText, nullptr, 16) == std::strtol(checksumOf(body).c_str(), nullptr, 16);
		const char answer = intact ? '+' : '-';
		writeAll(&answer, 1);

		if (!intact)
		{
			lastErrorMsg = "The GDB server sent a packet whose checksum did not match";
			return false;
		}

		/* Two kinds of packet are not answers: the console output the server
		   may interleave, and the notification it may send on its own. The
		   console one also starts with an 'O', which is the first letter of the
		   acknowledgement of a write - "OK". What tells them apart is the
		   payload: the console one carries the output in hex, and the 'K' of
		   "OK" is not a hex digit, so a write acknowledgement is kept while a
		   console packet is stepped over. */
		const bool isConsoleOutput = body.size() > 1 && body[0] == 'O' &&
									 std::all_of(body.begin() + 1, body.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
		const bool isNotification = !body.empty() && body[0] == '%';

		if (isConsoleOutput || isNotification)
			continue;

		reply = body;
		return true;
	}
}

bool GdbServerDebugProbe::startAcqusition(const DebugProbeSettings& probeSettings, std::vector<std::pair<uint32_t, uint8_t>>&, uint32_t)
{
	std::scoped_lock lock(mtx);

	lastErrorMsg = "";

	closeLink();

	if (probeSettings.ipAddress.empty())
	{
		lastErrorMsg = "No GDB server address was given!";
		logger->error(lastErrorMsg);
		logger->error("Open Options -> Acquisition settings while the VARIABLE VIEWER tab is active and fill in the GDB server address and port.");
		return false;
	}

	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char portText[16]{};
	std::snprintf(portText, sizeof(portText), "%u", probeSettings.port);

	addrinfo* resolved = nullptr;

	if (getaddrinfo(probeSettings.ipAddress.c_str(), portText, &hints, &resolved) != 0 || resolved == nullptr)
	{
		lastErrorMsg = "Could not resolve the GDB server address '" + probeSettings.ipAddress + "'";
		logger->error(lastErrorMsg);
		return false;
	}

	for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next)
	{
		const intptr_t opened = static_cast<intptr_t>(::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol));

		if (opened == noHandle)
			continue;

		/* Connected with a deadline rather than by waiting for the system's:
		   an address that answers nothing would otherwise hold the acquisition
		   thread for as long as the system feels like waiting. */
#ifdef _WIN32
		u_long nonBlocking = 1;
		ioctlsocket(static_cast<SOCKET>(opened), FIONBIO, &nonBlocking);
#else
		const int32_t flags = fcntl(static_cast<int>(opened), F_GETFL, 0);
		fcntl(static_cast<int>(opened), F_SETFL, flags | O_NONBLOCK);
#endif

		const int32_t connected = static_cast<int32_t>(
#ifdef _WIN32
			connect(static_cast<SOCKET>(opened), candidate->ai_addr, static_cast<int>(candidate->ai_addrlen))
#else
			::connect(static_cast<int>(opened), candidate->ai_addr, candidate->ai_addrlen)
#endif
		);

		if (connected != 0)
		{
			fd_set writable{};
			FD_ZERO(&writable);
			FD_SET(static_cast<int32_t>(opened), &writable);

			timeval timeout{};
			timeout.tv_sec = connectTimeoutMs / 1000;
			timeout.tv_usec = (connectTimeoutMs % 1000) * 1000;

#ifdef _WIN32
			const int32_t ready = select(0, nullptr, &writable, nullptr, &timeout);
#else
			const int32_t ready = select(static_cast<int>(opened) + 1, nullptr, &writable, nullptr, &timeout);
#endif

			if (ready <= 0)
			{
#ifdef _WIN32
				closesocket(static_cast<SOCKET>(opened));
#else
				::close(static_cast<int>(opened));
#endif
				continue;
			}
		}

		/* Back to waiting, and with a deadline on every read and write: a
		   server that stops answering mid-run should fail a read, not freeze
		   the run. */
#ifdef _WIN32
		nonBlocking = 0;
		ioctlsocket(static_cast<SOCKET>(opened), FIONBIO, &nonBlocking);

		DWORD timeoutMs = ioTimeoutMs;
		setsockopt(static_cast<SOCKET>(opened), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
		setsockopt(static_cast<SOCKET>(opened), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
		fcntl(static_cast<int>(opened), F_SETFL, flags);
#endif

		handle = opened;
		break;
	}

	freeaddrinfo(resolved);

	if (handle == noHandle)
	{
		lastErrorMsg = "Could not connect to the GDB server at " + probeSettings.ipAddress + ":" + std::to_string(probeSettings.port);
		logger->error(lastErrorMsg);
		logger->error("Check that a GDB server is running there. OpenOCD, or the server shipped with your probe, both serve this protocol.");
		return false;
	}

	isRunning = true;
	logger->info("Connected to the GDB server at {}:{}", probeSettings.ipAddress, probeSettings.port);

	/* What the server says it can take in one packet. It is a courtesy: a
	   server that does not answer the question is read in the default size. */
	if (sendPacket("qSupported"))
	{
		std::string supported;

		if (receivePacket(supported))
		{
			const size_t at = supported.find("PacketSize=");

			if (at != std::string::npos)
			{
				const uint32_t offered = static_cast<uint32_t>(std::strtoul(supported.c_str() + at + 11, nullptr, 16));

				/* One byte of a read comes back as two characters of hex, so a
				   packet of a given size carries a quarter of it in bytes. The
				   rest of the budget is left for the header. */
				if (offered > 0)
					packetSize = std::clamp(offered / 4, static_cast<uint32_t>(16), static_cast<uint32_t>(4096));
			}

			logger->info("GDB server packet size: {}", packetSize);
		}
	}

	return true;
}

bool GdbServerDebugProbe::stopAcqusition()
{
	std::scoped_lock lock(mtx);

	if (handle == noHandle)
		return true;

	/* Asked to detach rather than simply dropped: that is what tells the server
	   the target may run on, and a server left waiting for a client that has
	   gone is a target that stays halted. */
	sendPacket("D");

	closeLink();
	isRunning = false;
	logger->info("GDB server detached.");

	return true;
}

bool GdbServerDebugProbe::readMemory(uint32_t address, uint8_t* buf, uint32_t size)
{
	std::scoped_lock lock(mtx);

	if (handle == noHandle || !isRunning || size == 0)
		return false;

	char command[64]{};
	std::snprintf(command, sizeof(command), "m%x,%x", address, size);

	std::string reply;

	if (!sendPacket(command) || !receivePacket(reply))
		return false;

	if (reply.empty() || reply[0] == 'E')
	{
		lastErrorMsg = reply.empty() ? "The GDB server refused a memory read at 0x" + std::to_string(address)
									 : "The GDB server refused a memory read at 0x" + std::to_string(address) + " (" + reply + ")";
		return false;
	}

	if (reply.size() < static_cast<size_t>(size) * 2)
	{
		lastErrorMsg = "The GDB server answered a memory read with fewer bytes than were asked for";
		return false;
	}

	for (uint32_t i = 0; i < size; i++)
		buf[i] = byteFromHex(reply, static_cast<size_t>(i) * 2);

	return true;
}

bool GdbServerDebugProbe::writeMemory(uint32_t address, uint8_t* buf, uint32_t size)
{
	std::scoped_lock lock(mtx);

	if (handle == noHandle || !isRunning || size == 0)
		return false;

	char command[32]{};
	std::snprintf(command, sizeof(command), "M%x,%x:", address, size);

	std::string reply;

	if (!sendPacket(command + hexOf(buf, size)) || !receivePacket(reply))
		return false;

	if (reply != "OK")
	{
		lastErrorMsg = "The GDB server did not accept a write at 0x" + std::to_string(address) + " (" + reply + ")";
		return false;
	}

	return true;
}

bool GdbServerDebugProbe::readBlock(uint32_t address, uint8_t* buf, uint32_t size)
{
	/* The recorder buffer is downloaded this way, so a range is walked in
	   chunks rather than word by word. A GDB server takes a longer read than
	   the four bytes a single variable needs, and every packet costs a round
	   trip. */
	for (uint32_t done = 0; done < size;)
	{
		const uint32_t take = std::min(packetSize, size - done);

		if (!readMemory(address + done, buf + done, take))
			return false;

		done += take;
	}

	return true;
}
