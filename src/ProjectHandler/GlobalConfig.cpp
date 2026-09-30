#include "GlobalConfig.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "../commons.hpp"
#include "nlohmann/json.hpp"

using nlohmann::json;

static constexpr const char* separator = "/";

GlobalConfig::GlobalConfig(spdlog::logger* logger) : logger(logger)
{
}

std::string GlobalConfig::getGlobalConfigPath()
{
	std::string directory = getApplicationDataDirectory();

	if (directory.empty())
		return "";

	return directory + separator + "global.json";
}

bool GlobalConfig::load()
{
	std::string path = getGlobalConfigPath();

	if (path.empty())
	{
		logger->warn("Application data directory is unavailable, using default global settings");
		return false;
	}

	std::ifstream file(path);

	if (!file.is_open())
	{
		logger->info("No global config found at {}, using defaults", path);
		return false;
	}

	json root;

	try
	{
		file >> root;
	}
	catch (const std::exception& ex)
	{
		logger->error("Global config is not valid JSON ({}), using defaults", ex.what());
		return false;
	}

	/* A config written by a newer build may use fields we do not understand,
	   so only the flags we actually know about are read back. */
	settings.formatVersion = root.value("formatVersion", formatVersion);

	if (root.contains("autoSave"))
	{
		const auto& autoSave = root.at("autoSave");
		settings.autoSaveEnabled = autoSave.value("enabled", settings.autoSaveEnabled);
		settings.autoSaveIntervalSeconds = autoSave.value("intervalSeconds", settings.autoSaveIntervalSeconds);
	}

	if (root.contains("interface"))
	{
		const auto& interface = root.at("interface");
		settings.fontScale = interface.value("fontScale", settings.fontScale);
	}

	if (root.contains("recentProjects") && root.at("recentProjects").is_array())
	{
		settings.recentProjects.clear();
		for (const auto& entry : root.at("recentProjects"))
		{
			if (entry.is_string())
				settings.recentProjects.push_back(entry.get<std::string>());
		}
	}

	if (root.contains("api"))
	{
		const auto& api = root.at("api");
		settings.mcpEnabled = api.value("enabled", settings.mcpEnabled);
		settings.mcpPreferredPort = api.value("preferredPort", settings.mcpPreferredPort);
		settings.apiWritesEnabled = api.value("writesEnabled", settings.apiWritesEnabled);
	}

	if (root.contains("plotExport"))
	{
		const auto& plotExport = root.at("plotExport");
		settings.plotExport.askForLocation = plotExport.value("askForLocation", settings.plotExport.askForLocation);
		settings.plotExport.directory = plotExport.value("directory", settings.plotExport.directory);
		settings.plotExport.fileName = plotExport.value("fileName", settings.plotExport.fileName);
		settings.plotExport.incrementFileName = plotExport.value("incrementFileName", settings.plotExport.incrementFileName);
	}

	if (root.contains("flashing"))
	{
		const auto& flashing = root.at("flashing");
		settings.flash.command = flashing.value("command", settings.flash.command);
		settings.flash.file = flashing.value("file", settings.flash.file);
		settings.flash.useElfFile = flashing.value("useElfFile", settings.flash.useElfFile);
		settings.flash.timeoutEnabled = flashing.value("timeoutEnabled", settings.flash.timeoutEnabled);
		settings.flash.timeoutSeconds = flashing.value("timeoutSeconds", settings.flash.timeoutSeconds);
	}

	if (settings.flash.timeoutSeconds < FlashingService::minimumTimeoutSeconds || settings.flash.timeoutSeconds > FlashingService::maximumTimeoutSeconds)
		settings.flash.timeoutSeconds = 60;

	if (settings.autoSaveIntervalSeconds < 10)
		settings.autoSaveIntervalSeconds = 10;

	/* A port below 1024 needs privileges on some systems, and 0 would leave the
	   server to pick one that the client then cannot predict. */
	if (settings.mcpPreferredPort < 1024)
		settings.mcpPreferredPort = 7777;

	logger->info("Global config loaded from {}, {} recent project(s)", path, settings.recentProjects.size());

	return true;
}

bool GlobalConfig::save()
{
	std::string path = getGlobalConfigPath();

	if (path.empty())
	{
		logger->warn("Application data directory is unavailable, global settings not saved");
		return false;
	}

	std::error_code errorCode;
	std::filesystem::path filePath(path);

	if (!filePath.parent_path().empty())
		std::filesystem::create_directories(filePath.parent_path(), errorCode);

	json root;
	root["formatVersion"] = formatVersion;
	root["autoSave"] = {{"enabled", settings.autoSaveEnabled}, {"intervalSeconds", settings.autoSaveIntervalSeconds}};
	root["interface"] = {{"fontScale", settings.fontScale}};
	root["api"] = {{"enabled", settings.mcpEnabled},
				   {"preferredPort", settings.mcpPreferredPort},
				   {"writesEnabled", settings.apiWritesEnabled}};
	root["flashing"] = {{"command", settings.flash.command},
						{"file", settings.flash.file},
						{"useElfFile", settings.flash.useElfFile},
						{"timeoutEnabled", settings.flash.timeoutEnabled},
						{"timeoutSeconds", settings.flash.timeoutSeconds}};
	root["plotExport"] = {{"askForLocation", settings.plotExport.askForLocation},
						  {"directory", settings.plotExport.directory},
						  {"fileName", settings.plotExport.fileName},
						  {"incrementFileName", settings.plotExport.incrementFileName}};
	root["recentProjects"] = settings.recentProjects;

	std::ofstream file(path, std::ios::trunc);

	if (!file.is_open())
	{
		logger->error("Failed to open global config for writing: {}", path);
		return false;
	}

	file << root.dump(2);

	if (!file.good())
	{
		logger->error("Failed to write global config: {}", path);
		return false;
	}

	return true;
}

GlobalConfig::Settings& GlobalConfig::getSettings()
{
	return settings;
}

void GlobalConfig::addRecentProject(const std::string& path)
{
	if (path.empty())
		return;

	std::error_code errorCode;
	std::string normalized = std::filesystem::absolute(path, errorCode).lexically_normal().string();

	if (errorCode)
		normalized = path;

	settings.recentProjects.erase(std::remove(settings.recentProjects.begin(), settings.recentProjects.end(), normalized), settings.recentProjects.end());
	settings.recentProjects.insert(settings.recentProjects.begin(), normalized);

	if (settings.recentProjects.size() > maxRecentProjects)
		settings.recentProjects.resize(maxRecentProjects);
}

void GlobalConfig::removeMissingRecentProjects()
{
	std::error_code errorCode;

	settings.recentProjects.erase(std::remove_if(settings.recentProjects.begin(), settings.recentProjects.end(),
												 [&errorCode](const std::string& path)
												 { return !std::filesystem::exists(path, errorCode); }),
								  settings.recentProjects.end());
}
