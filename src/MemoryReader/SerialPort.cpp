#include "SerialTransport.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <windows.h>
#else
	#include <dirent.h>
	#include <fcntl.h>
	#include <poll.h>
	#include <termios.h>
	#include <unistd.h>
#endif

namespace
{
#ifdef _WIN32
	/* The registry is where Windows publishes the ports it has, and asking it
	   costs nothing, unlike opening every candidate to see whether it answers. */
	std::vector<std::string> portsFromRegistry()
	{
		std::vector<std::string> ports;

		HKEY key = nullptr;

		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS)
			return ports;

		char valueName[256] = {};
		char value[256] = {};

		for (DWORD index = 0;; index++)
		{
			DWORD valueNameSize = sizeof(valueName);
			DWORD valueSize = sizeof(value);
			DWORD type = 0;

			if (RegEnumValueA(key, index, valueName, &valueNameSize, nullptr, &type, reinterpret_cast<LPBYTE>(value), &valueSize) != ERROR_SUCCESS)
				break;

			/* A REG_SZ value carries its terminating zero in the size. */
			if (type == REG_SZ && valueSize > 1)
				ports.emplace_back(value, valueSize - 1);
		}

		RegCloseKey(key);
		return ports;
	}

	/* COM10 sorts before COM9 as text, which reads as a mistake in a list. */
	bool comparePortNames(const std::string& left, const std::string& right)
	{
		const bool leftIsCom = left.rfind("COM", 0) == 0;
		const bool rightIsCom = right.rfind("COM", 0) == 0;

		if (leftIsCom && rightIsCom)
		{
			const long leftNumber = std::strtol(left.c_str() + 3, nullptr, 10);
			const long rightNumber = std::strtol(right.c_str() + 3, nullptr, 10);

			if (leftNumber != rightNumber)
				return leftNumber < rightNumber;
		}

		return left < right;
	}
#endif

#ifndef _WIN32
	speed_t baudrateConstant(uint32_t baudrate)
	{
		switch (baudrate)
		{
			case 1200:
				return B1200;
			case 2400:
				return B2400;
			case 4800:
				return B4800;
			case 9600:
				return B9600;
			case 19200:
				return B19200;
			case 38400:
				return B38400;
			case 57600:
				return B57600;
			case 115200:
				return B115200;
			case 230400:
				return B230400;
	#ifdef B460800
			case 460800:
				return B460800;
	#endif
	#ifdef B921600
			case 921600:
				return B921600;
	#endif
			default:
				return B115200;
		}
	}
#endif

	class SystemTransport : public serial::ITransport
	{
	   public:
		SystemTransport() = default;
		~SystemTransport() override { close(); }

		bool open(const std::string& portName, uint32_t baudrate) override;
		void close() override;
		bool isOpen() const override;
		bool write(const uint8_t* data, size_t size) override;
		size_t read(uint8_t* data, size_t size, uint32_t timeoutMs) override;
		void discardInput() override;
		std::string getLastError() const override { return lastError; }

	   private:
#ifdef _WIN32
		HANDLE handle = INVALID_HANDLE_VALUE;
#else
		int handle = -1;
#endif
		std::string lastError;
	};

#ifdef _WIN32
	bool SystemTransport::open(const std::string& portName, uint32_t baudrate)
	{
		close();

		if (portName.empty())
		{
			lastError = "No serial port was selected.";
			return false;
		}

		/* A port above COM9 is only reachable through the device namespace. */
		std::string path = portName;

		if (path.rfind("\\\\", 0) != 0)
			path = "\\\\.\\" + path;

		handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

		if (handle == INVALID_HANDLE_VALUE)
		{
			lastError = "Cannot open serial port '" + portName + "' (error " + std::to_string(GetLastError()) + ").";
			return false;
		}

		SetupComm(handle, 65536, 65536);

		DCB state = {};
		state.DCBlength = sizeof(DCB);

		if (!GetCommState(handle, &state))
		{
			lastError = "Cannot read the line settings of '" + portName + "'.";
			close();
			return false;
		}

		state.BaudRate = baudrate;
		state.ByteSize = 8;
		state.Parity = NOPARITY;
		state.StopBits = ONESTOPBIT;
		state.fBinary = TRUE;
		state.fParity = FALSE;
		state.fOutxCtsFlow = FALSE;
		state.fOutxDsrFlow = FALSE;
		state.fOutX = FALSE;
		state.fInX = FALSE;
		state.fDsrSensitivity = FALSE;
		/* A USB virtual port on the target side often only starts sending once
		   the host raises these two lines, so they are left asserted. */
		state.fDtrControl = DTR_CONTROL_ENABLE;
		state.fRtsControl = RTS_CONTROL_ENABLE;

		if (!SetCommState(handle, &state))
		{
			lastError = "Cannot apply the line settings of '" + portName + "' (error " + std::to_string(GetLastError()) + ").";
			close();
			return false;
		}

		discardInput();
		lastError.clear();
		return true;
	}

	void SystemTransport::close()
	{
		if (handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(handle);
			handle = INVALID_HANDLE_VALUE;
		}
	}

	bool SystemTransport::isOpen() const
	{
		return handle != INVALID_HANDLE_VALUE;
	}

	bool SystemTransport::write(const uint8_t* data, size_t size)
	{
		if (!isOpen())
		{
			lastError = "The serial port is not open.";
			return false;
		}

		size_t written = 0;

		while (written < size)
		{
			DWORD chunk = 0;

			if (!WriteFile(handle, data + written, static_cast<DWORD>(size - written), &chunk, nullptr))
			{
				lastError = "Writing to the serial port failed (error " + std::to_string(GetLastError()) + ").";
				return false;
			}

			if (chunk == 0)
			{
				lastError = "The serial port accepted no data.";
				return false;
			}

			written += chunk;
		}

		return true;
	}

	size_t SystemTransport::read(uint8_t* data, size_t size, uint32_t timeoutMs)
	{
		if (!isOpen())
			return 0;

		/* A total timeout with a zero multiplier makes the read return once the
		   requested number of bytes arrive or the budget runs out, whichever
		   comes first. */
		COMMTIMEOUTS timeouts = {};
		timeouts.ReadIntervalTimeout = 0;
		timeouts.ReadTotalTimeoutMultiplier = 0;
		timeouts.ReadTotalTimeoutConstant = timeoutMs;
		SetCommTimeouts(handle, &timeouts);

		DWORD collected = 0;

		if (!ReadFile(handle, data, static_cast<DWORD>(size), &collected, nullptr))
		{
			const DWORD error = GetLastError();

			/* Running out of time is how a short reply ends up reported, and the
			   probe turns it into its own message. */
			if (error != ERROR_TIMEOUT && error != ERROR_SEM_TIMEOUT)
				lastError = "Reading from the serial port failed (error " + std::to_string(error) + ").";
		}

		return collected;
	}

	void SystemTransport::discardInput()
	{
		if (isOpen())
			PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
	}
#else
	bool SystemTransport::open(const std::string& portName, uint32_t baudrate)
	{
		close();

		if (portName.empty())
		{
			lastError = "No serial port was selected.";
			return false;
		}

		handle = ::open(portName.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);

		if (handle < 0)
		{
			lastError = "Cannot open serial port '" + portName + "': " + std::strerror(errno) + ".";
			return false;
		}

		termios settings = {};

		if (tcgetattr(handle, &settings) != 0)
		{
			lastError = "Cannot read the line settings of '" + portName + "': " + std::strerror(errno) + ".";
			close();
			return false;
		}

		cfmakeraw(&settings);

		const speed_t speed = baudrateConstant(baudrate);
		cfsetispeed(&settings, speed);
		cfsetospeed(&settings, speed);

		settings.c_cflag |= (CLOCAL | CREAD);
		settings.c_cflag &= ~CSIZE;
		settings.c_cflag |= CS8;
		settings.c_cflag &= ~PARENB;
		settings.c_cflag &= ~CSTOPB;
	#ifdef CRTSCTS
		settings.c_cflag &= ~CRTSCTS;
	#endif
		settings.c_cc[VMIN] = 0;
		settings.c_cc[VTIME] = 0;

		if (tcsetattr(handle, TCSANOW, &settings) != 0)
		{
			lastError = "Cannot apply the line settings of '" + portName + "': " + std::strerror(errno) + ".";
			close();
			return false;
		}

		discardInput();
		lastError.clear();
		return true;
	}

	void SystemTransport::close()
	{
		if (handle >= 0)
		{
			::close(handle);
			handle = -1;
		}
	}

	bool SystemTransport::isOpen() const
	{
		return handle >= 0;
	}

	bool SystemTransport::write(const uint8_t* data, size_t size)
	{
		if (!isOpen())
		{
			lastError = "The serial port is not open.";
			return false;
		}

		size_t written = 0;

		while (written < size)
		{
			const ssize_t chunk = ::write(handle, data + written, size - written);

			if (chunk < 0)
			{
				if (errno == EAGAIN || errno == EINTR)
					continue;

				lastError = "Writing to the serial port failed: " + std::string(std::strerror(errno)) + ".";
				return false;
			}

			written += static_cast<size_t>(chunk);
		}

		return true;
	}

	size_t SystemTransport::read(uint8_t* data, size_t size, uint32_t timeoutMs)
	{
		if (!isOpen())
			return 0;

		pollfd descriptor = {};
		descriptor.fd = handle;
		descriptor.events = POLLIN;

		const int ready = ::poll(&descriptor, 1, static_cast<int>(timeoutMs));

		if (ready <= 0)
			return 0;

		const ssize_t collected = ::read(handle, data, size);
		return collected > 0 ? static_cast<size_t>(collected) : 0;
	}

	void SystemTransport::discardInput()
	{
		if (isOpen())
		{
			tcflush(handle, TCIOFLUSH);
		}
	}
#endif
}  // namespace

namespace serial
{
	std::unique_ptr<ITransport> createSystemTransport()
	{
		return std::make_unique<SystemTransport>();
	}

	std::vector<std::string> enumeratePorts()
	{
		std::vector<std::string> ports;

#ifdef _WIN32
		ports = portsFromRegistry();
		std::sort(ports.begin(), ports.end(), comparePortNames);
#else
		const char* directories[] = {"/dev", "/dev/serial/by-id"};

		for (const char* directory : directories)
		{
			DIR* handle = opendir(directory);

			if (handle == nullptr)
				continue;

			while (dirent* entry = readdir(handle))
			{
				const std::string name = entry->d_name;
				const bool looksLikeAPort = name.rfind("ttyUSB", 0) == 0 || name.rfind("ttyACM", 0) == 0 ||
											name.rfind("ttyAMA", 0) == 0 || name.rfind("ttyS", 0) == 0 ||
											name.rfind("rfcomm", 0) == 0;

				if (looksLikeAPort)
					ports.push_back(std::string(directory) + "/" + name);
			}

			closedir(handle);
		}

		std::sort(ports.begin(), ports.end());
		ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
#endif

		return ports;
	}
}  // namespace serial
