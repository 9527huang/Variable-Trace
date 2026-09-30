#include "commons.hpp"

#include <cstdlib>
#include <string>

std::string toLower(std::string str)
{
	std::transform(str.begin(), str.end(), str.begin(),
				   [](unsigned char c)
				   { return std::tolower(c); });

	return str;
}

std::string getApplicationDataDirectory()
{
#if defined(__APPLE__) || defined(_UNIX)
	const char* base = std::getenv("HOME");
#elif _WIN32
	const char* base = std::getenv("APPDATA");
#else
#error "Your system is not supported!"
#endif

	if (base == nullptr)
		return "";

	return std::string(base) + "/Variable-Trace";
}