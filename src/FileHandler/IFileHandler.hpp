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
	/* `defaultPath` is the directory the dialog opens on and `defaultName` the
	   name it starts with. Both are optional; an empty one leaves the choice to
	   the dialog. An empty result means the dialog was dismissed or could not
	   be shown, and the two are not told apart. */
	virtual std::string saveFile(std::vector<Filter>&& filters, const std::string& defaultPath = "", const std::string& defaultName = "") = 0;
	virtual std::string openDirectory(std::vector<Filter>&& filters) = 0;
};

#endif
