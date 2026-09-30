#ifndef _NFDFILEHANDLER_HPP
#define _NFDFILEHANDLER_HPP

#include <string>
#include <utility>
#include <vector>

#include "IFileHandler.hpp"

class NFDFileHandler : public IFileHandler
{
   public:
	bool
	init() override;
	bool deinit() override;
	std::string openFile(std::vector<Filter>&& filters) override;
	std::string saveFile(std::vector<Filter>&& filters, const std::string& defaultPath = "", const std::string& defaultName = "") override;
	std::string openDirectory(std::vector<Filter>&& filters) override;

   private:
	enum class handleType
	{
		SAVE,
		OPEN,
		OPENDIR
	};
	std::string handleFile(handleType type, std::vector<Filter>& filters, const std::string& defaultPath = "", const std::string& defaultName = "");
};

#endif
