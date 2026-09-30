#include "SerialDriverLayout.hpp"

namespace serial
{
	namespace
	{
		uint32_t readU32(const uint8_t* data, size_t offset)
		{
			return static_cast<uint32_t>(data[offset]) |
				   (static_cast<uint32_t>(data[offset + 1]) << 8) |
				   (static_cast<uint32_t>(data[offset + 2]) << 16) |
				   (static_cast<uint32_t>(data[offset + 3]) << 24);
		}
	}  // namespace

	DriverSettings parseDriverSettings(const uint8_t* data, size_t length)
	{
		DriverSettings settings;

		if (data == nullptr || length < driverSettingsSize)
			return settings;

		settings.version = readU32(data, 0);
		settings.revision = readU32(data, 4);
		settings.maxVariables = readU32(data, 8);

		return settings;
	}

	std::string describeDriverSettings(const DriverSettings& settings)
	{
		if (settings.version == 0)
			return "Serial driver not detected.";

		std::string text = "Serial driver " + std::to_string(settings.version) + "." + std::to_string(settings.revision);

		if (settings.maxVariables != 0)
			text += ", up to " + std::to_string(settings.maxVariables) + " variables per read";

		if (!isSupported(settings))
			text += " (a version this build was not written against)";

		return text;
	}

	bool isSupported(const DriverSettings& settings)
	{
		if (settings.version != supportedVersion)
			return false;

		return settings.revision <= supportedRevision;
	}
}  // namespace serial
