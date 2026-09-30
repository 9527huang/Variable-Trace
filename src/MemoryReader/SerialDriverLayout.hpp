#ifndef _SERIALDRIVERLAYOUT_HPP
#define _SERIALDRIVERLAYOUT_HPP

#include <cstddef>
#include <cstdint>
#include <string>

/*
 * What the target side serial driver says about itself.
 *
 * The driver keeps a settings struct in the target memory and fills it in at
 * compile time. A host that reads those three numbers knows which revision of
 * the driver it is talking to and how wide a single bulk read may be, without
 * having to be told by the person at the keyboard and without having to guess
 * from a failure.
 *
 * The struct is the first thing the driver publishes, and it is deliberately
 * small: three 32 bit fields, in the order the driver writes them.
 *
 *   0   version        4 bytes
 *   4   revision       4
 *   8   maxVariables   4
 *
 * maxVariables is the one that matters at run time. It is the number of entries
 * a single bulk read may carry; the driver drops a frame that asks for more,
 * rather than truncating it, so a host that assumes a larger number than the
 * firmware was built with gets nothing back at all.
 */

namespace serial
{
	struct DriverSettings
	{
		uint32_t version = 0;
		uint32_t revision = 0;
		uint32_t maxVariables = 0;
	};

	constexpr size_t driverSettingsSize = 12;

	/* The versions this host knows how to talk to. A larger version may have
	   moved a field, and reading a struct at the wrong offsets produces numbers
	   rather than an error, so the comparison is worth making. */
	constexpr uint32_t supportedVersion = 1;
	constexpr uint32_t supportedRevision = 2;

	/* Reads the struct out of a buffer of target memory. A buffer that is too
	   short yields the values a zeroed struct would, which the caller can
	   recognise because a driver never reports a version of zero. */
	DriverSettings parseDriverSettings(const uint8_t* data, size_t length);

	/* One line naming the revision and the limit, for a window. */
	std::string describeDriverSettings(const DriverSettings& settings);

	/* Whether the revision is one this host was written against. */
	bool isSupported(const DriverSettings& settings);
}  // namespace serial

#endif
