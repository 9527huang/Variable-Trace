#ifndef _FILEHANDLER_HPP
#define _FILEHANDLER_HPP

#include <string>
#include <utility>
#include <vector>

class IFileHandler
{
   public:
	/* Display name of a filter and the matching extensions, for example
	   {"Project files", "mcvproj"}. Several entries produce a dropdown in the
	   system dialog, which is what lets the project open dialog offer the
	   current and the legacy format side by side. */
	using Filter = std::pair<std::string, std::string>;

	virtual ~IFileHandler() = default;
	virtual bool init() = 0;
	virtual bool deinit() = 0;
	virtual std::string openFile(std::vector<Filter>&& filters) = 0;
	virtual std::string saveFile(std::vector<Filter>&& filters) = 0;
	virtual std::string openDirectory(std::vector<Filter>&& filters) = 0;
};

#endif
