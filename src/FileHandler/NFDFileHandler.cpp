#include <NFDFileHandler.hpp>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "nfd.h"

bool NFDFileHandler::init()
{
	return NFD_Init() != NFD_ERROR;
}

bool NFDFileHandler::deinit()
{
	NFD_Quit();
	return true;
}

std::string NFDFileHandler::openFile(std::vector<Filter>&& filters)
{
	return handleFile(handleType::OPEN, filters);
}

std::string NFDFileHandler::saveFile(std::vector<Filter>&& filters)
{
	return handleFile(handleType::SAVE, filters);
}

std::string NFDFileHandler::openDirectory(std::vector<Filter>&& filters)
{
	return handleFile(handleType::OPENDIR, filters);
}

std::string NFDFileHandler::handleFile(handleType type, std::vector<Filter>& filters)
{
	nfdchar_t* outPath = nullptr;

	/* The dialog keeps pointers into these entries while it is open, so the
	   strings have to outlive the call and the vector must not be resized. */
	std::vector<nfdfilteritem_t> filterItems;
	filterItems.reserve(filters.size());

	for (const Filter& filter : filters)
		filterItems.push_back({filter.first.c_str(), filter.second.c_str()});

	const nfdfilteritem_t* filterData = filterItems.empty() ? nullptr : filterItems.data();
	const nfdfiltersize_t filterCount = static_cast<nfdfiltersize_t>(filterItems.size());

	nfdresult_t result = NFD_ERROR;

	if (type == handleType::SAVE)
		result = NFD_SaveDialog(&outPath, filterData, filterCount, NULL, NULL);
	else if (type == handleType::OPEN)
		result = NFD_OpenDialog(&outPath, filterData, filterCount, NULL);
	else if (type == handleType::OPENDIR)
		result = NFD_PickFolder(&outPath, NULL);

	if (result == NFD_OKAY)
	{
		std::string path = std::string(outPath);
		std::replace(path.begin(), path.end(), '\\', '/');
		NFD_FreePath(outPath);
		return path;
	}
	return std::string("");
}
