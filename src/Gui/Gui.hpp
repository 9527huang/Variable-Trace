#ifndef _GUI_HPP
#define _GUI_HPP

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>

#include "ConfigHandler.hpp"
#include "ElfTarget.hpp"
#include "FlashingService.hpp"
#include "GlobalConfig.hpp"
#include "GuiPlotEdit.hpp"
#include "GuiPlotsTree.hpp"
#include "GuiVarTable.hpp"
#include "GuiVariablesEdit.hpp"
#include "GuiWritePlanner.hpp"
#include "IDebugProbe.hpp"
#include "IElfParser.hpp"
#include "IFileHandler.hpp"
#include "ImguiPlugins.hpp"
#include "JlinkDebugProbe.hpp"
#include "JlinkTraceProbe.hpp"
#include "Plot.hpp"
#include "PlotGroupHandler.hpp"
#include "Popup.hpp"
#include "ProjectHandler.hpp"
#include "TraceDataHandler.hpp"
#include "VariableHandler.hpp"
#include "ViewerDataHandler.hpp"
#include "WritePlanner.hpp"
#include "imgui.h"
#include "implot.h"

/* The API server is only held by pointer, so its headers stay out of this one. */
namespace mcp
{
	class Server;
	class Bridge;
}  // namespace mcp

/* Same reasoning for the recorder: only a pointer to it is stored here. */
namespace recorder
{
	class RecorderHandler;
}  // namespace recorder

struct McpContext;

class Gui
{
   public:
	Gui(PlotHandler* plotHandler, VariableHandler* variableHandler, ConfigHandler* configHandler, ProjectHandler* projectHandler, GlobalConfig* globalConfig, PlotGroupHandler* plotGroupHandler, IFileHandler* fileHandler, PlotHandler* tracePlotHandler, ViewerDataHandler* viewerDataHandler, TraceDataHandler* traceDataHandler, std::atomic<bool>& done, std::mutex* mtx, spdlog::logger* logger, std::string& projectPath);
	~Gui();

   private:
	static constexpr bool showDemoWindow = false;

	const std::map<DataHandlerBase::State, std::string> viewerStateMap{{DataHandlerBase::State::RUN, "RUNNING"}, {DataHandlerBase::State::STOP, "STOPPED"}};

	std::thread threadHandle;
	PlotHandler* plotHandler;
	VariableHandler* variableHandler;
	ConfigHandler* configHandler;
	ProjectHandler* projectHandler;
	GlobalConfig* globalConfig;
	PlotGroupHandler* plotGroupHandler;

	std::string projectConfigPath;
	std::string projectElfPath;
	std::chrono::steady_clock::time_point lastAutoSaveTime{};
	bool showAcqusitionSettingsWindow = false;
	bool showAboutWindow = false;
	bool showPreferencesWindow = false;
	bool showSelectVariablesWindow = false;
	bool showRecorderWindow = false;
	bool showFlashingSettingsWindow = false;
	bool showFlashOutputWindow = false;
	bool showWritePlannerWindow = false;

	IFileHandler* fileHandler;
	PlotHandler* tracePlotHandler;
	ViewerDataHandler* viewerDataHandler;
	TraceDataHandler* traceDataHandler;

	std::shared_ptr<IDebugProbe> stlinkProbe;
	std::shared_ptr<IDebugProbe> jlinkProbe;
	std::shared_ptr<IDebugProbe> serialProbe;
	std::shared_ptr<IDebugProbe> debugProbeDevice;
	std::vector<std::string> devicesList{};
	const std::string noDevices = "No debug probes found!";

	std::shared_ptr<ITraceProbe> stlinkTraceProbe;
	std::shared_ptr<ITraceProbe> jlinkTraceProbe;
	std::shared_ptr<ITraceProbe> traceProbeDevice;

	std::atomic<bool>& done;

	Popup popup;
	Popup acqusitionErrorPopup;

	enum class ActiveViewType : uint8_t
	{
		VarViewer = 0,
		TraceViewer = 1,
	};

	ActiveViewType activeView = ActiveViewType::VarViewer;

	std::mutex* mtx;

	spdlog::logger* logger;

	std::shared_ptr<PlotEditWindow> plotEditWindow;
	std::shared_ptr<VariableTableWindow> variableTable;
	std::shared_ptr<PlotsTree> plotsTree;

	/* ------------------------------------------------------------ flashing */
	/* Declared before the server on purpose: a run the server started has to be
	   waited for after the server has been shut down, and members are destroyed
	   in reverse declaration order. The destructor joins the child process. */
	std::unique_ptr<FlashingService> flasher;
	Popup flashPopup;
	/* Which of the two buttons started the run that is on screen, so that the
	   test panel can say "Not tested" instead of showing the output of a flash
	   the user started from the toolbar. */
	enum class FlashKind : uint8_t
	{
		None = 0,
		Test = 1,
		Real = 2,
	};
	FlashKind lastFlashKind = FlashKind::None;

	/* Copy of the programmer output, refreshed only when a line has arrived.
	   Both panels read it, so they cannot show different text. */
	std::vector<std::string> flashOutputLines;
	size_t flashOutputRevision = 0;

	/* The write plans are edited in a window of their own and travel in the
	   project file, so the model is owned here rather than by the window. */
	std::shared_ptr<WritePlanner> writePlanner;
	std::shared_ptr<WritePlannerWindow> writePlannerWindow;

	/* Declared in this order on purpose. The server holds bare pointers to the
	   bridge and to the context, and its destructor reaches into the bridge, so
	   both have to outlive it. Members are destroyed in reverse declaration
	   order, which puts the server first only if it is declared last. */
	std::unique_ptr<mcp::Bridge> mcpBridge;
	std::unique_ptr<McpContext> mcpContext;
	std::unique_ptr<mcp::Server> mcpServer;

	/* The recorder runs over whichever probe is currently selected, so it holds
	   no connection of its own and borrows the one the viewer acquired. */
	std::unique_ptr<recorder::RecorderHandler> recorderHandler;

	/* The API refresh needs an ELF parser of its own, because the one behind the
	   variable table belongs to that window. It follows the same setting, so the
	   two always read the file the same way. */
	std::shared_ptr<IElfParser> mcpElfParser;
	std::string mcpElfParserName;

	/* Set when the file that was just chosen belongs to a target whose
	   addresses are counted differently from the way the current parser counts
	   them. The suggestion waits here until the frame that draws the window,
	   because the file picker runs outside it. */
	bool showC2000ParserSuggestion = false;
	bool c2000SuggestionOpened = false;
	std::string c2000SuggestionPath;

	/* Set when a project has been loaded and the server should come up on the new
	   model. The restart is deferred because a tool call runs on the interface
	   thread and stopping the server from inside one would join the very thread
	   that is serving it. */
	bool mcpRestartPending = false;

   private:
	void mainThread(std::string externalPath);
	void drawMenu();
	/* The state of the probe behind the tab that is showing, plus, for the
	   variable viewer, the rate the acquisition is reaching. Drawn at the right
	   end of the main menu bar. */
	void drawConnectionIndicator();
	void drawStartButton(DataHandlerBase* activeDataHandler);
	void drawDebugProbes();
	void drawTraceProbes();
	void drawUpdateAddressesFromElf();
	void drawAcqusitionSettingsWindow(ActiveViewType type);
	void drawAcqusitionSettingsScope(ActiveViewType type);
	void acqusitionSettingsViewer();

	template <typename Settings>
	void drawLoggingSettings(PlotHandler* handler, Settings& settings);
	void drawElfSettings(ViewerDataHandler::Settings& settings);

	void drawAboutWindow();
	void drawPreferencesWindow();
	void acqusitionSettingsTrace();

	void drawPlots();
	void drawPlotCurve(std::shared_ptr<Plot> plot);
	void drawPlotBar(std::shared_ptr<Plot> plot);
	void drawPlotTable(std::shared_ptr<Plot> plot);
	void drawPlotXY(std::shared_ptr<Plot> plot);
	void handleMarkers(uint32_t id, Plot::Marker& marker, ImPlotRect plotLimits, std::function<void()> activeCallback);
	void handleDragRect(uint32_t id, Plot::DragRect& dragRect, ImPlotRect plotLimits);
	void dragAndDropPlot(std::shared_ptr<Plot> plot);

	void showQuestionBox(const char* id, const char* question, std::function<void()> onYes, std::function<void()> onNo, std::function<void()> onCancel);
	void askShouldSaveOnExit(bool shouldOpenPopup);
	void askShouldSaveOnNew(bool shouldOpenPopup);
	ProjectData collectProjectData();
	void applyProjectData(const ProjectData& data);
	void applyTracePlots(const std::vector<ProjectTracePlot>& tracePlots);
	void selectProbeDevices();
	/* Maps an IDebugProbe::Probe value to the object that serves it, so the
	   index to object relation is written down in one place only. */
	std::shared_ptr<IDebugProbe> probeForType(uint32_t type) const;
	std::string rebaseElfPath(const std::string& newProjectPath);
	void updateAutoSave();
	void drawOpenRecentMenu();
	bool saveProject();
	bool saveProjectAs();
	void showChangeFormatPopup(const char* text, Plot& plt, const std::string& name);
	bool openElfFile();
	bool openLogDirectory(std::string& logDirectory);
	void checkShortcuts();
	bool checkElfFileChanged();
	bool openProject(std::string externalPath = "");
	bool importLegacyProject(const std::string& path);

	/* ------------------------------------------------------- symbol readers */
	/* Rebuilds the API's parser when the setting names a different one. */
	void syncMcpElfParser();
	/* Looks at the header of a symbol file and, when it says the target counts
	   memory in 16 bit words, asks the user to switch to the parser that counts
	   the same way. Curiosity, not a decision: a file that is read with the
	   wrong parser yields addresses rather than an error. */
	void inspectElfTarget(const std::string& path);
	void drawC2000ParserSuggestion();

	/* True when the current parser builds a command line out of the path, which
	   is the only case in which a space in the path is a problem. */
	bool parserNeedsPathWithoutSpaces() const;

	/* ------------------------------------------------------------ recorder */
	void drawRecorderWindow();
	/* Looks the two recorder structures up in the variable table and hands their
	   addresses to the recorder. Resolved on demand rather than kept in step
	   with every import, so an import that happens later is picked up by the
	   next detect. Returns false when either name is missing. */
	bool syncRecorderSymbols();

	/* ------------------------------------------------------------ flashing */
	/* The button that runs the configured programmer. Labelled with a spinner
	   while a run is in progress. */
	void drawFlashButton();
	/* What the programmer prints, plus the button that cuts the run short. */
	void drawFlashOutputWindow();
	/* Firmware file, command template, inactivity timeout, and a button that
	   tries the command once without a full flash. */
	void drawFlashingSettingsWindow();
	/* The file the {file} marker is replaced with: either the one that was
	   picked or the *.elf of the acquisition settings. */
	std::string resolveFirmwareFile() const;
	/* The programmer output, copied from the service only when it has grown. */
	const std::vector<std::string>& flashOutput();
	/* Stops the acquisition and starts the programmer. `test` decides which
	   panel reports the outcome. */
	void startFlash(bool test);

	/* --------------------------------------------------------- write planner */
	void drawWritePlannerWindow();

	/* ------------------------------------------------------------ API server */
	void setupMcpServer();
	void applyMcpSettings();

	/* Drops the whole model and starts an empty project. Shared by the New menu
	   entry and the API, so the two cannot drift apart. */
	void newProject();
	bool setElfPathFromApi(const std::string& path, bool relative);
	bool refreshVariableAddressesFromElf(const std::string& elfPath);
	void drawSettingsSwo();
	void drawIndicatorsSwo();
	void drawPlotsSwo();
	void drawPlotCurveSwo(Plot* plot, ScrollingBuffer<double>& time, std::map<std::string, std::shared_ptr<Plot::Series>>& seriesMap, bool first);
	void drawPlotsTreeSwo();

	bool openWebsite(const char* url);
};

#endif
