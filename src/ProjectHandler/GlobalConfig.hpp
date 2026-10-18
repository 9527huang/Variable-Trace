#ifndef _GLOBALCONFIG_HPP
#define _GLOBALCONFIG_HPP

#include "RtosTableModel.hpp"

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

		/* "dark" or "light". Stored as a name rather than an index so that
		   adding a third theme later cannot silently turn an existing setting
		   into a different one. */
		std::string theme = "dark";

		/* "english" or "chinese", by name for the same reason as the theme. A
		   name that is not recognised is read as English where it is applied,
		   so a config from a build offering a language this one does not know
		   still opens in a language the reader has. */
		std::string language = "english";

		std::vector<std::string> recentProjects{};

		/* The API server is off until it is switched on. It accepts connections
		   from any local process and can reach the target through the probe, so it
		   is opt-in. The write permission is a second switch that only allows
		   writing to the target, never reading it. */
		bool mcpEnabled = false;
		uint16_t mcpPreferredPort = 7777;
		bool apiWritesEnabled = false;

		/* The command channel is a second way in, for callers that cannot speak
		   HTTP. It runs the same tools as the API server, so it is off until it
		   is switched on and it listens on the loopback interface only. */
		bool commandChannelEnabled = false;
		uint16_t commandChannelPreferredPort = 7808;

		/*
		 * The task view. Off until someone asks for it.
		 *
		 * The plan's final default is on, but the feature is still being built
		 * and there is no window to open it from yet, so a build that turned
		 * it on would spend probe time on rounds nobody can look at. This is
		 * the switch the plan requires to be read once at startup and never
		 * during a run, because turning it off has to leave the program
		 * exactly as it was before the feature existed.
		 */
		bool rtosEnabled = false;

		/*
		 * The stack alert: a task whose stack is this empty or less is marked
		 * in the table. Zero turns it off.
		 *
		 * A number rather than a switch and a number, because zero is the
		 * switch: no stack is ever less than nothing free, so a threshold of
		 * zero can only mean that nobody wants to be told.
		 */
		uint32_t rtosStackAlertFreePercent = 20;

		/* The task view's own set-up: sort order, filters, and the timeline's
		   window. Kept here because it is about this machine's reader, not
		   about the project being debugged. */
		RtosViewSettings rtosView{};

		/* Which carrier the target's switch records arrive on: the ring in its
		   memory, or the trace unit. "ring" unless someone asks for the other,
		   because asking for SWO means writing the target's trace registers -
		   not something to do to a board that was never wired for it. */
		std::string rtosEventsTransport = "ring";

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
