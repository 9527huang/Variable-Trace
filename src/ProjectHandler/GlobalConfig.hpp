#ifndef _GLOBALCONFIG_HPP
#define _GLOBALCONFIG_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "FlashingService.hpp"
#include "PlotExport.hpp"
#include "spdlog/spdlog.h"

/*
 * Settings that belong to the installation rather than to a single project.
 *
 * Stored next to the log files in the per-user application data directory, so
 * that opening a project never depends on this file being present.
 */
class GlobalConfig
{
   public:
	static constexpr uint32_t formatVersion = 1;
	static constexpr size_t maxRecentProjects = 10;

	struct Settings
	{
		uint32_t formatVersion = GlobalConfig::formatVersion;
		bool autoSaveEnabled = true;
		uint32_t autoSaveIntervalSeconds = 120;
		float fontScale = 1.0f;
		std::vector<std::string> recentProjects{};

		/* The API server is off until it is switched on. It accepts connections
		   from any local process and can reach the target through the probe, so it
		   is opt-in. The write permission is a second switch that only allows
		   writing to the target, never reading it. */
		bool mcpEnabled = false;
		uint16_t mcpPreferredPort = 7777;
		bool apiWritesEnabled = false;

		/* Flashing. The programmer and the arguments it needs belong to the
		   machine rather than to the project being debugged, so the command
		   template lives here and not in the project file. */
		FlashingService::FlashSettings flash{};

		/* Plot export. An export directory belongs to the machine in the same
		   way the programmer does, so this lives here as well. */
		plotExport::Settings plotExport{};
	};

	explicit GlobalConfig(spdlog::logger* logger);

	/* Never throws. A missing or malformed file leaves the defaults in place. */
	bool load();
	bool save();

	Settings& getSettings();

	/* Moves the path to the front, drops duplicates and truncates the list. */
	void addRecentProject(const std::string& path);

	/* Drops entries whose file no longer exists. */
	void removeMissingRecentProjects();

	static std::string getGlobalConfigPath();

   private:
	Settings settings{};
	spdlog::logger* logger;
};

#endif
