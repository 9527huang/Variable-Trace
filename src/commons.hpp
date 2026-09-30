#ifndef COMMONS_HPP
#define COMMONS_HPP

#include <algorithm>
#include <string>

#if defined(unix) || defined(__unix__) || defined(__unix)
#define _UNIX
#endif

std::string toLower(std::string str);

/* Per-user application data directory, without a trailing separator.
   Windows: %APPDATA%/Variable-Trace, Unix: $HOME/Variable-Trace.
   Returns an empty string when the environment variable is not set. */
std::string getApplicationDataDirectory();

#endif