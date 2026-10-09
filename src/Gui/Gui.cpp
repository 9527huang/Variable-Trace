#include "Gui.hpp"

#include <imgui.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <future>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "PlotHandler.hpp"
#include "RecorderHandler.hpp"
#include "SerialDebugProbe.hpp"
#include "Statistics.hpp"
#include "StlinkDebugProbe.hpp"
#include "glfw3.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"

#include "../commons.hpp"

#include "I18n.hpp"
#include "McpBridge.hpp"
#include "McpContext.hpp"
#include "McpServer.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

Gui::Gui(PlotHandler* plotHandler, VariableHandler* variableHandler, ConfigHandler* configHandler, ProjectHandler* projectHandler, GlobalConfig* globalConfig, PlotGroupHandler* plotGroupHandler, IFileHandler* fileHandler, PlotHandler* tracePlotHandler, ViewerDataHandler* viewerDataHandler, TraceDataHandler* traceDataHandler, std::atomic<bool>& done, std::mutex* mtx, spdlog::logger* logger, std::string& projectPath) : plotHandler(plotHandler), variableHandler(variableHandler), configHandler(configHandler), projectHandler(projectHandler), globalConfig(globalConfig), plotGroupHandler(plotGroupHandler), fileHandler(fileHandler), tracePlotHandler(tracePlotHandler), viewerDataHandler(viewerDataHandler), traceDataHandler(traceDataHandler), done(done), mtx(mtx), logger(logger)
{
	lastAutoSaveTime = std::chrono::steady_clock::now();

	/* The recorder reads and writes whichever probe the viewer acquired, so it
	   needs nothing beyond the logger to exist. It is built before the plots
	   tree because that tree offers the entries that make a recorder group, and
	   it greys them out while the recorder itself is switched off. */
	recorderHandler = std::make_unique<recorder::RecorderHandler>(logger);

	plotEditWindow = std::make_shared<PlotEditWindow>(plotHandler, plotGroupHandler, variableHandler, viewerDataHandler);
	plotsTree = std::make_shared<PlotsTree>(viewerDataHandler, plotHandler, plotGroupHandler, variableHandler, plotEditWindow, fileHandler, recorderHandler.get(), logger);
	variableTable = std::make_shared<VariableTableWindow>(viewerDataHandler, plotHandler, variableHandler, &projectElfPath, &projectConfigPath, logger);

	variableHandler->renameCallback = [&](std::string oldName, std::string newName)
	{
		for (std::shared_ptr<Plot> plt : *this->plotHandler)
			plt->renameSeries(oldName, newName);
	};

	/* Deleting a variable is one change with two halves: the series that name it
	   on a plot are bare pointers, so they have to go while the object is still
	   there, and then the object itself. VariableHandler::erase calls this
	   between the two, with the model lock already held, which is what lets the
	   variable be deleted while the acquisition thread is reading the model.

	   Nothing here asks for that lock again. */
	variableHandler->removeSeriesCallback = [&](const std::string& name)
	{
		for (PlotHandler* handler : {this->plotHandler, this->tracePlotHandler})
		{
			if (handler == nullptr)
				continue;

			for (std::shared_ptr<Plot> plt : *handler)
				plt->removeSeries(name);
		}
	};

	/* The programmer is a child process, so it needs no probe either. */
	flasher = std::make_unique<FlashingService>(logger);

	writePlanner = std::make_shared<WritePlanner>();
	writePlannerWindow = std::make_shared<WritePlannerWindow>(writePlanner.get(), variableHandler);

	setupMcpServer();

	/* Started last: the drawing code reads every member above, so the thread has
	   to find them already built rather than race the constructor for them. */
	threadHandle = std::thread(&Gui::mainThread, this, projectPath);
}

Gui::~Gui()
{
	/* The server goes first: its connection threads hand work to the GUI thread
	   through the bridge, so shutting the bridge down while calls can still arrive
	   would leave those threads waiting for their full timeout. */
	if (mcpServer != nullptr)
		mcpServer->stop();

	if (mcpBridge != nullptr)
		mcpBridge->shutdown();

	if (threadHandle.joinable())
		threadHandle.join();
}

static void glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static float getContentScale(GLFWwindow* window)
{
	float xscale;
	float yscale;
	glfwGetWindowContentScale(window, &xscale, &yscale);
	return (xscale + yscale) / 2.0f;
}

void Gui::mainThread(std::string externalPath)
{
	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit())
		return;

	GLFWwindow* window = glfwCreateWindow(1500, 1000, (std::string("Variable-Trace | ") + projectConfigPath).c_str(), NULL, NULL);
	if (window == NULL)
		return;
	glfwMakeContextCurrent(window);
	glfwMaximizeWindow(window);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImPlot::CreateContext();

	GuiHelper::contentScale = getContentScale(window);

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	/* Before the font, because which glyphs are needed depends on it, and
	   before the first frame, because the atlas is built by then. */
	applyLanguage();
	setupFonts();

	/* Keep the window layout next to the logs rather than beside the executable.
	 *
	 * Left alone, ImGui opens "imgui.ini" and resolves it against the process
	 * working directory. The layout then depends on where the program was
	 * started from - a shortcut, a build directory and an installed copy each
	 * get their own file, and the first run in a directory that has none starts
	 * from a layout with the panes unassigned. It also means the layout cannot
	 * be found again once the directory changes.
	 *
	 * The string has to outlive the frame loop, because ImGui keeps the pointer
	 * instead of copying it, hence static. */
	static const std::string iniPath = getApplicationDataDirectory() + "/imgui.ini";
	if (!getApplicationDataDirectory().empty())
		io.IniFilename = iniPath.c_str();

	applyTheme();

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init("#version 130");

	ImGuiWindowClass window_class;
	window_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;

	fileHandler->init();

	jlinkProbe = std::make_shared<JlinkDebugProbe>(logger);
	stlinkProbe = std::make_shared<StlinkDebugProbe>(logger);
	serialProbe = std::make_shared<SerialDebugProbe>(logger);
	debugProbeDevice = stlinkProbe;
	viewerDataHandler->setDebugProbe(debugProbeDevice);

	jlinkTraceProbe = std::make_shared<JlinkTraceProbe>(logger);
	stlinkTraceProbe = std::make_shared<StlinkTraceProbe>(logger);
	traceProbeDevice = stlinkTraceProbe;
	traceDataHandler->setDebugProbe(traceProbeDevice);

	if (!externalPath.empty())
		openProject(externalPath);

	while (!done)
	{
		if (glfwGetWindowAttrib(window, GLFW_ICONIFIED))
		{
			/* Requests waiting in the bridge are run here as well: a minimized
			   window would otherwise let every call expire on its timeout. */
			mcpBridge->processPendingTasks();
			glfwWaitEvents();
			continue;
		}

		if (glfwGetWindowAttrib(window, GLFW_FOCUSED) || (traceDataHandler->getStateImmediate() == DataHandlerBase::State::RUN) || (viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN))
			glfwSwapInterval(1);
		else
			glfwSwapInterval(4);

		glfwSetWindowTitle(window, (std::string("Variable-Trace - ") + projectConfigPath).c_str());
		glfwPollEvents();
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();

		/* Changing the language changes which glyphs are needed, so the atlas
		   has to be rebuilt and its texture replaced. ImGui only allows that
		   outside a frame, and the dialog that asks for it runs inside one, so
		   the request is recorded there and carried out here. */
		if (fontRebuildPending)
		{
			fontRebuildPending = false;
			rebuildFonts();
		}

		ImGui::NewFrame();
		ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());
		ensureDockLayout(dockspaceId);

		/* Tool calls arrive on connection threads and are run here, before the
		   frame reads the model, so a change made through the API is visible in
		   the same frame that applies it. */
		mcpBridge->processPendingTasks();

		/* A tool call may have loaded a project. The server is brought up again
		   once no request is being served, which is the only point where stopping
		   it cannot join the thread that would serve this call. */
		if (mcpRestartPending)
		{
			mcpRestartPending = false;
			applyMcpSettings();
		}

		if (showDemoWindow)
			ImPlot::ShowDemoWindow();

		if (glfwWindowShouldClose(window))
		{
			viewerDataHandler->setState(DataHandlerBase::State::STOP);
			traceDataHandler->setState(DataHandlerBase::State::STOP);

			if (projectHandler->isSavingRequired(collectProjectData()))
				askShouldSaveOnExit(glfwWindowShouldClose(window));
			else
				done = true;
		}
		glfwSetWindowShouldClose(window, done);
		checkShortcuts();
		updateAutoSave();

		drawMenu();
		drawAboutWindow();
		drawPreferencesWindow();
		drawPlotExportDialog();
		drawRecorderWindow();
		drawFlashOutputWindow();
		drawFlashingSettingsWindow();
		drawWritePlannerWindow();
		drawC2000ParserSuggestion();

		if (ImGui::Begin(I18n::windowTitle("Trace Viewer").c_str()))
		{
			activeView = ActiveViewType::TraceViewer;
			drawAcqusitionSettingsWindow(activeView);
			ImGui::SetNextWindowClass(&window_class);
			if (ImGui::Begin(I18n::windowTitle("Trace Plots").c_str()))
				drawPlotsSwo();
			ImGui::End();
			drawStartButton(traceDataHandler);
			drawSettingsSwo();
			drawIndicatorsSwo();
			drawPlotsTreeSwo();
		}
		ImGui::End();

		if (ImGui::Begin(I18n::windowTitle("Var Viewer").c_str()))
		{
			activeView = ActiveViewType::VarViewer;
			drawAcqusitionSettingsWindow(activeView);
			drawStartButton(viewerDataHandler);

			/* A flash needs the acquisition to stop, so the button that starts
			   one sits right under the button that starts the other. */
			drawFlashButton();

			variableTable->draw();
			plotsTree->draw();
			plotEditWindow->draw();
			ImGui::SetNextWindowClass(&window_class);
			if (ImGui::Begin(I18n::windowTitle("Plots").c_str()))
				drawPlots();
			ImGui::End();
		}
		ImGui::End();

		/* The image save runs after the frame has been drawn, so a message it
		   produced is held until the next frame, which is the first one with a
		   window to open a popup on. */
		if (!pendingPlotImageMessage.empty())
		{
			popup.show(pendingPlotImageTitle.c_str(), pendingPlotImageMessage.c_str(), pendingPlotImageSeconds);
			pendingPlotImageMessage.clear();
		}

		popup.handle();
		flashPopup.handle();

		// Rendering
		ImGui::Render();
		int display_w, display_h;
		glfwGetFramebufferSize(window, &display_w, &display_h);
		glViewport(0, 0, display_w, display_h);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		/* The back buffer still holds this frame and is about to be handed to
		   the screen, which is the only moment its pixels can be read. */
		capturePlotImages();

		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			GLFWwindow* backup_current_context = glfwGetCurrentContext();
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
			glfwMakeContextCurrent(backup_current_context);
		}

		glfwSwapBuffers(window);
	}

	logger->info("Exiting GUI main thread");

	globalConfig->save();

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(window);
	glfwTerminate();
	fileHandler->deinit();
}

void Gui::drawMenu()
{
	bool shouldSaveOnClose = false;
	bool shouldSaveOnNew = false;
	ImGui::BeginMainMenuBar();

	bool active = !(viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN || traceDataHandler->getStateImmediate() == DataHandlerBase::State::RUN);

	if (ImGui::BeginMenu(I18n::tr("File")))
	{
		/* Loading a project replaces every variable and plot, so it is only
		   offered while no acquisition is running. */
		if (ImGui::MenuItem(I18n::tr("New"), "Ctrl+N", false, active))
			shouldSaveOnNew = true;

		if (ImGui::MenuItem(I18n::tr("Open"), "Ctrl+O", false, active))
			openProject();

		drawOpenRecentMenu();

		if (ImGui::MenuItem(I18n::tr("Save"), "Ctrl+S", false, (!projectConfigPath.empty())))
			saveProject();

		if (ImGui::MenuItem(I18n::tr("Save As..")))
			saveProjectAs();

		/* Saving the plots does not touch the model, so it stays available
		   while an acquisition is running. That is when the plots are worth
		   keeping in the first place. */
		if (ImGui::MenuItem(I18n::tr("Save Plots to *.png"), "Ctrl+P", false, true))
			requestPlotImages();

		if (ImGui::MenuItem(I18n::tr("Quit")))
			shouldSaveOnClose = true;

		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu(I18n::tr("Options")))
	{
		ImGui::MenuItem(I18n::tr("Acquisition settings..."), NULL, &showAcqusitionSettingsWindow, active);

		/* The programmer and the firmware file belong to the machine rather than
		   to the project, which is why they are reached from here and not from
		   the project file dialog. */
		ImGui::MenuItem(I18n::tr("Flashing"), NULL, &showFlashingSettingsWindow, true);

		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu(I18n::tr("Window")))
	{
		ImGui::MenuItem(I18n::tr("Recorder"), NULL, &showRecorderWindow, active);
		ImGui::MenuItem(I18n::tr("Flash Output"), NULL, &showFlashOutputWindow, true);
		ImGui::MenuItem(I18n::tr("Write Planner"), NULL, &showWritePlannerWindow, active);
		ImGui::MenuItem(I18n::tr("Preferences"), NULL, &showPreferencesWindow, active);
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu(I18n::tr("Help")))
	{
		ImGui::MenuItem(I18n::tr("About"), NULL, &showAboutWindow, active);
		ImGui::EndMenu();
	}

	/* Drawn while the bar is still the current window, because it reads the
	   place the menu titles ended at as its left boundary. */
	drawConnectionIndicator();

	ImGui::EndMainMenuBar();
	askShouldSaveOnExit(shouldSaveOnClose);
	askShouldSaveOnNew(shouldSaveOnNew);
}

/* One label and value line of the connection tooltip. The value column is at a
   fixed offset so that the rows line up without a table. */
static void drawIndicatorRow(const char* label, const std::string& value)
{
	ImGui::TextUnformatted(label);
	ImGui::SameLine(135.0f * GuiHelper::contentScale);
	ImGui::TextUnformatted(value.c_str());
}

void Gui::drawConnectionIndicator()
{
	const bool isVariableView = (activeView == ActiveViewType::VarViewer);

	/* The two tabs drive two separate probes, so the indicator follows the tab
	   that is showing rather than reporting a single global state. */
	DataHandlerBase* handler = isVariableView ? static_cast<DataHandlerBase*>(viewerDataHandler) : static_cast<DataHandlerBase*>(traceDataHandler);

	static const char* const probeNames[] = {"STLINK", "JLINK", "SERIAL"};
	const uint32_t probeType = isVariableView ? viewerDataHandler->getProbeSettings().debugProbe : traceDataHandler->getProbeSettings().debugProbe;
	const size_t probeIndex = (static_cast<size_t>(probeType) < IM_ARRAYSIZE(probeNames)) ? static_cast<size_t>(probeType) : 0;
	const char* const probeName = probeNames[probeIndex];

	std::string serialNumber;
	std::string targetName;
	uint32_t speedkHz = 0;
	uint32_t baudrate = 0;
	bool serialPort = false;
	bool hasTargetName = false;
	bool highSpeed = false;
	bool keepConnection = false;

	if (isVariableView)
	{
		const IDebugProbe::DebugProbeSettings settings = viewerDataHandler->getProbeSettings();

		serialNumber = settings.serialNumber;
		targetName = settings.device;
		speedkHz = settings.speedkHz;
		baudrate = settings.baudrate;
		serialPort = (settings.debugProbe == IDebugProbe::Probe::Serial);
		hasTargetName = (settings.debugProbe == IDebugProbe::Probe::Jlink);
		highSpeed = (settings.mode == IDebugProbe::Mode::HSS);
		keepConnection = settings.keepConnection;
	}
	else
	{
		const ITraceProbe::TraceProbeSettings settings = traceDataHandler->getProbeSettings();

		serialNumber = settings.serialNumber;
		targetName = settings.device;
		speedkHz = settings.speedkHz;
		hasTargetName = (settings.debugProbe == 1);
		keepConnection = settings.keepConnection;
	}

	/* The requested state is stored the moment the button is pressed, so the
	   pending flag is what keeps "the probe is being opened" apart from "the
	   data flows". */
	const bool transitioning = handler->isTransitionPending();
	const bool acquired = (handler->getStateImmediate() == DataHandlerBase::State::RUN);

	/* The four names the state takes, gathered so that the width below can be
	   measured over all of them: the segment keeps the width of the longest, or
	   it would grow and shrink as the state changes and shove the group beside
	   it along the bar. */
	enum StateName
	{
		DisconnectedName,
		ConnectedName,
		ConnectingName,
		DisconnectingName
	};

	const char* stateTexts[] = {I18n::tr("Disconnected"), I18n::tr("Connected"), I18n::tr("Connecting"), I18n::tr("Disconnecting")};

	const char* stateText = stateTexts[DisconnectedName];
	ImVec4 stateFace = GuiHelper::red();
	ImVec4 stateFaceHovered = GuiHelper::redLight();
	ImVec4 stateFaceActive = GuiHelper::redLightDim();

	/* What this segment reports is the link to the target, not whether samples
	   are flowing: the two part company once the connection is asked to outlive
	   the run, and then the probe is still there to be read from. */
	const bool linkUp = acquired || handler->isProbeHeld();

	if (linkUp)
	{
		stateText = stateTexts[ConnectedName];
		stateFace = GuiHelper::green();
		stateFaceHovered = GuiHelper::greenLight();
		stateFaceActive = GuiHelper::greenLightDim();
	}

	if (transitioning)
	{
		/* Starting a run opens the link and ending one closes it - unless it was
		   asked to stay open, in which case nothing about the link changes and
		   there is no state of its own to report. */
		const bool openingLink = acquired;
		const bool closingLink = !acquired && !keepConnection;

		if (openingLink || closingLink)
		{
			stateText = openingLink ? stateTexts[ConnectingName] : stateTexts[DisconnectingName];
			stateFace = GuiHelper::orange();
			stateFaceHovered = GuiHelper::orangeLight();
			stateFaceActive = GuiHelper::orangeLightDim();
		}
	}

	const float scale = GuiHelper::contentScale;
	const float textPadding = 9.0f * scale;
	const float gap = 10.0f * scale;
	const float margin = 6.0f * scale;
	const float height = ImGui::GetFrameHeight();

	/* Both segments keep the width of their longest text, so that the group does
	   not move while the state or the rate changes. The longest of the four is
	   measured in the language in force, since a translated name is generally
	   not the length of the English one it stands in for. */
	float stateWidth = 0.0f;

	for (const char* name : stateTexts)
		stateWidth = std::max(stateWidth, ImGui::CalcTextSize(name).x);

	stateWidth += 2.0f * textPadding;

	const float probeWidth = ImGui::CalcTextSize(probeName).x + 2.0f * textPadding;

	char samplingText[64] = "";
	float samplingWidth = 0.0f;

	if (isVariableView)
	{
		/* Formatted here rather than by the helper that draws it, because the
		   width has to be known before the row is laid out. */
		std::snprintf(samplingText, sizeof(samplingText), I18n::tr("sampling: %.2f Hz"), viewerDataHandler->getAverageSamplingFrequency());
		samplingWidth = ImGui::CalcTextSize(I18n::tr("sampling: 1000000.00 Hz")).x;
	}

	const bool withSampling = (samplingText[0] != '\0');
	const float rateWidth = withSampling ? samplingWidth + gap : 0.0f;

	/* The menu titles are drawn first, so where the bar cursor sits now is the
	   left edge this group must not cross. */
	const float titlesEndX = ImGui::GetCursorPosX() + gap;
	float startX = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - margin - rateWidth - stateWidth - probeWidth;

	if (startX < titlesEndX && withSampling)
	{
		/* The connection state is worth more than the rate, so the rate is the
		   one that is given up when the window is too narrow for both. */
		samplingText[0] = '\0';
		startX += rateWidth;
	}

	if (startX < titlesEndX)
		return;

	ImGui::SetCursorPosX(startX);

	bool hovered = false;
	bool pressed = false;

	ImGui::PushStyleColor(ImGuiCol_Button, stateFace);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, stateFaceHovered);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, stateFaceActive);

	pressed |= ImGui::Button(stateText, ImVec2(stateWidth, height));
	hovered |= ImGui::IsItemHovered();

	ImGui::PopStyleColor(3);

	/* No gap between the two, so they read as one control with a coloured half
	   and a named half. */
	ImGui::SameLine(0.0f, 0.0f);

	ImGui::PushStyleColor(ImGuiCol_Button, GuiHelper::neutral());
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GuiHelper::neutralLight());
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, GuiHelper::neutralLight());

	pressed |= ImGui::Button(probeName, ImVec2(probeWidth, height));
	hovered |= ImGui::IsItemHovered();

	ImGui::PopStyleColor(3);

	if (pressed)
		showAcqusitionSettingsWindow = true;

	if (samplingText[0] != '\0')
	{
		ImGui::SameLine(0.0f, gap);

		/* Right aligned inside the width reserved for it, so extra digits grow
		   towards the left and nothing else in the bar moves. */
		const float valueWidth = ImGui::CalcTextSize(samplingText).x;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + samplingWidth - valueWidth);
		ImGui::TextUnformatted(samplingText);
	}

	if (!hovered)
		return;

	ImGui::BeginTooltip();
	ImGui::TextUnformatted(I18n::tr(isVariableView ? "Variable Viewer acquisition" : "Trace Viewer acquisition"));
	ImGui::Separator();

	drawIndicatorRow(I18n::tr("State"), stateText);
	drawIndicatorRow(I18n::tr("Probe"), probeName);

	if (serialPort)
	{
		drawIndicatorRow(I18n::tr("Serial port"), serialNumber.empty() ? "-" : serialNumber);
		drawIndicatorRow(I18n::tr("Baudrate"), std::to_string(baudrate) + " baud");
	}
	else
	{
		drawIndicatorRow(I18n::tr("Serial number"), serialNumber.empty() ? "-" : serialNumber);
		drawIndicatorRow(I18n::tr("SWD speed"), std::to_string(speedkHz) + " kHz");
	}

	if (hasTargetName)
	{
		drawIndicatorRow(I18n::tr("Target name"), targetName.empty() ? "-" : targetName);
		drawIndicatorRow(I18n::tr("Mode"), I18n::tr(highSpeed ? "HSS" : "NORMAL"));
	}

	/* The reason the last start failed. Without it a disconnected chip says
	   nothing about whether the probe, the target or a setting is at fault. */
	const std::string readerError = handler->getLastReaderError();

	if (!readerError.empty())
	{
		ImGui::Separator();
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360.0f * GuiHelper::contentScale);
		ImGui::TextUnformatted(readerError.c_str());
		ImGui::PopTextWrapPos();
	}

	ImGui::Separator();
	ImGui::TextUnformatted(I18n::tr("Click to open the acquisition settings."));
	ImGui::EndTooltip();
}

void Gui::drawOpenRecentMenu()
{
	GlobalConfig::Settings& settings = globalConfig->getSettings();

	if (!ImGui::BeginMenu(I18n::tr("Open Recent"), !settings.recentProjects.empty()))
		return;

	for (const std::string& path : settings.recentProjects)
	{
		const std::string fileName = std::filesystem::path(path).filename().string();

		if (ImGui::MenuItem(fileName.c_str()))
		{
			openProject(path);
			break;
		}

		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", path.c_str());
	}

	ImGui::Separator();

	if (ImGui::MenuItem(I18n::tr("Clear list")))
	{
		settings.recentProjects.clear();
		globalConfig->save();
	}

	ImGui::EndMenu();
}

void Gui::updateAutoSave()
{
	GlobalConfig::Settings& settings = globalConfig->getSettings();

	if (!settings.autoSaveEnabled || projectConfigPath.empty())
	{
		lastAutoSaveTime = std::chrono::steady_clock::now();
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	const auto elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - lastAutoSaveTime).count();

	if (elapsedSeconds < static_cast<int64_t>(settings.autoSaveIntervalSeconds))
		return;

	lastAutoSaveTime = now;

	if (!projectHandler->isSavingRequired(collectProjectData()))
		return;

	if (saveProject())
		logger->info("Auto saved project: {}", projectConfigPath);
	else
		logger->error("Auto save failed for project: {}", projectConfigPath);
}

ProjectData Gui::collectProjectData()
{
	ProjectData data;

	data.elfPath = projectElfPath;

	const ViewerDataHandler::Settings viewerSettings = viewerDataHandler->getSettings();
	data.viewer.sampleFrequencyHz = viewerSettings.sampleFrequencyHz;
	data.viewer.maxPoints = viewerSettings.maxPoints;
	data.viewer.maxViewportPoints = viewerSettings.maxViewportPoints;
	data.viewer.refreshAddressesOnElfChange = viewerSettings.refreshAddressesOnElfChange;
	data.viewer.stopAcquisitionOnElfChange = viewerSettings.stopAcqusitionOnElfChange;
	data.viewer.loggingEnabled = viewerSettings.shouldLog;
	data.viewer.logDirectory = viewerSettings.logFilePath;
	data.viewer.gdbCommand = viewerSettings.gdbCommand;
	data.viewer.elfParser = viewerSettings.elfParser;
	data.viewer.ofd2000Command = viewerSettings.ofd2000Command;

	const IDebugProbe::DebugProbeSettings debugProbeSettings = viewerDataHandler->getProbeSettings();
	data.viewer.probe.type = debugProbeSettings.debugProbe;
	data.viewer.probe.serialNumber = debugProbeSettings.serialNumber;
	data.viewer.probe.targetName = debugProbeSettings.device;
	data.viewer.probe.mode = static_cast<uint32_t>(debugProbeSettings.mode);
	data.viewer.probe.speedKHz = debugProbeSettings.speedkHz;
	data.viewer.probe.baudrate = debugProbeSettings.baudrate;
	data.viewer.probe.keepConnection = debugProbeSettings.keepConnection;

	const TraceDataHandler::Settings traceSettings = traceDataHandler->getSettings();
	data.trace.coreFrequency = traceSettings.coreFrequency;
	data.trace.tracePrescaler = traceSettings.tracePrescaler;
	data.trace.maxPoints = traceSettings.maxPoints;
	data.trace.maxViewportPointsPercent = traceSettings.maxViewportPointsPercent;
	data.trace.triggerChannel = traceSettings.triggerChannel;
	data.trace.triggerLevel = traceSettings.triggerLevel;
	data.trace.shouldReset = traceSettings.shouldReset;
	data.trace.timeout = traceSettings.timeout;
	data.trace.loggingEnabled = traceSettings.shouldLog;
	data.trace.logDirectory = traceSettings.logFilePath;

	const ITraceProbe::TraceProbeSettings traceProbeSettings = traceDataHandler->getProbeSettings();
	data.trace.probe.type = traceProbeSettings.debugProbe;
	data.trace.probe.serialNumber = traceProbeSettings.serialNumber;
	data.trace.probe.targetName = traceProbeSettings.device;
	data.trace.probe.speedKHz = traceProbeSettings.speedkHz;
	data.trace.probe.keepConnection = traceProbeSettings.keepConnection;

	for (std::shared_ptr<Plot> plt : *tracePlotHandler)
	{
		ProjectTracePlot tracePlot;
		tracePlot.name = plt->getName();
		tracePlot.alias = plt->getAlias();
		tracePlot.visibility = plt->getVisibility();
		tracePlot.domain = static_cast<uint8_t>(plt->getDomain());
		tracePlot.varType = static_cast<uint8_t>(plt->getTraceVarType());

		auto traceVar = traceDataHandler->traceVars.find(tracePlot.name);

		if (traceVar != traceDataHandler->traceVars.end())
			tracePlot.color = traceVar->second->getColorU32();

		data.tracePlots.push_back(tracePlot);
	}

	/* Write plans are stored by name, so collecting them is a plain copy of the
	   model rather than a walk over the variables. */
	for (const std::string& name : writePlanner->getNames())
	{
		const WritePlanner::Plan plan = writePlanner->getPlan(name);

		ProjectWritePlan entry{};
		entry.name = plan.name;
		entry.variable = plan.variable;

		for (const WritePlanner::Step& step : plan.steps)
			entry.steps.push_back(ProjectWriteStep{step.time, step.value});

		data.writePlans.push_back(entry);
	}

	return data;
}

void Gui::applyTracePlots(const std::vector<ProjectTracePlot>& tracePlots)
{
	tracePlotHandler->removeAllPlots();
	traceDataHandler->traceVars.clear();

	for (const ProjectTracePlot& tracePlot : tracePlots)
	{
		auto plot = tracePlotHandler->addPlot(tracePlot.name);
		plot->setVisibility(tracePlot.visibility);
		plot->setDomain(static_cast<Plot::Domain>(tracePlot.domain));

		if (plot->getDomain() == Plot::Domain::ANALOG)
			plot->setTraceVarType(static_cast<Plot::TraceVarType>(tracePlot.varType));

		plot->setAlias(tracePlot.alias);

		auto traceVar = std::make_shared<Variable>(tracePlot.name);
		traceVar->setColor(tracePlot.color);
		traceDataHandler->traceVars[tracePlot.name] = traceVar;

		plot->addSeries(traceVar.get());
		plot->getSeries(tracePlot.name)->visible = true;

		logger->info("Adding trace plot: {}", tracePlot.name);
	}
}

void Gui::drawStartButton(DataHandlerBase* activeDataHandler)
{
	bool shouldDisableButton = (!devicesList.empty() && devicesList.front() == noDevices);
	ImGui::BeginDisabled(shouldDisableButton);

	DataHandlerBase::State state = activeDataHandler->getStateImmediate();

	if (state == DataHandlerBase::State::RUN)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, GuiHelper::green());
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GuiHelper::greenLight());
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, GuiHelper::greenLightDim());
	}
	else if (state == DataHandlerBase::State::STOP)
	{
		if (activeDataHandler->getLastReaderError() != "")
		{
			ImGui::PushStyleColor(ImGuiCol_Button, GuiHelper::red());
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GuiHelper::redLight());
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, GuiHelper::redLightDim());
		}
		else
		{
			ImGui::PushStyleColor(ImGuiCol_Button, GuiHelper::orange());
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GuiHelper::orangeLight());
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, GuiHelper::orangeLightDim());
		}
	}

	if ((ImGui::Button((std::string(I18n::tr(viewerStateMap.at(state).c_str())) + " " + activeDataHandler->getLastReaderError()).c_str(), ImVec2(-1, 50 * GuiHelper::contentScale)) ||
		 (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) &&
		!shouldDisableButton)
	{
		if (state == DataHandlerBase::State::STOP)
		{
			logger->info("Start clicked!");
			plotHandler->eraseAllPlotData();
			tracePlotHandler->eraseAllPlotData();
			activeDataHandler->setState(DataHandlerBase::State::RUN);
		}
		else
		{
			logger->info("Stop clicked!");
			activeDataHandler->setState(DataHandlerBase::State::STOP);
		}
	}

	ImGui::PopStyleColor(3);
	ImGui::EndDisabled();
}

void Gui::drawAcqusitionSettingsWindow(ActiveViewType type)
{
	if (showAcqusitionSettingsWindow)
		ImGui::OpenPopup(I18n::windowTitle("Acqusition Settings").c_str());

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(950 * GuiHelper::contentScale, 600 * GuiHelper::contentScale));
	if (ImGui::BeginPopupModal(I18n::windowTitle("Acqusition Settings").c_str(), &showAcqusitionSettingsWindow, 0))
	{
		drawAcqusitionSettingsScope(type);

		if (type == ActiveViewType::VarViewer)
			acqusitionSettingsViewer();
		else if (type == ActiveViewType::TraceViewer)
			acqusitionSettingsTrace();

		acqusitionErrorPopup.handle();

		const float buttonHeight = 25.0f * GuiHelper::contentScale;
		ImGui::SetCursorPos(ImVec2(0, ImGui::GetWindowSize().y - buttonHeight / 2.0f - ImGui::GetFrameHeightWithSpacing()));

		if (ImGui::Button(I18n::tr("Done"), ImVec2(-1, buttonHeight)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			showAcqusitionSettingsWindow = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}

void Gui::applyTheme()
{
	const GlobalConfig::Settings& settings = globalConfig->getSettings();

	GuiHelper::currentTheme = GuiHelper::themeFromName(settings.theme);

	ImGuiStyle& style = ImGui::GetStyle();

	/* Put the sizes back to the defaults before a palette goes on.
	 *
	 * The colour presets only write style.Colors; every size field (padding,
	 * spacing, scrollbar, rounding) keeps whatever value it already had. Scaling
	 * after that multiplies the current numbers, so a second call scales sizes
	 * that were scaled once already - switching the theme back and forth made
	 * every widget grow a little on each switch. Assigning a fresh ImGuiStyle
	 * resets the sizes to the values its constructor defines, and that
	 * constructor also applies the dark palette, so both bands start from the
	 * same numbers. */
	style = ImGuiStyle();

	if (GuiHelper::currentTheme == GuiHelper::Theme::Light)
	{
		ImGui::StyleColorsLight();
		ImPlot::StyleColorsLight();
	}
	else
	{
		ImGui::StyleColorsDark();
		ImPlot::StyleColorsDark();
	}

	/* Scaled once, from the defaults, so the outcome depends on the content
	   scale alone and not on how often the theme has been switched. */
	style.ScaleAllSizes(GuiHelper::contentScale);

	/* The popup is the one surface the presets leave translucent, which reads
	   as a smudge over a plot on a light background. Both palettes get an
	   opaque popup so a menu is legible wherever it lands. */
	style.Colors[ImGuiCol_PopupBg] = GuiHelper::currentTheme == GuiHelper::Theme::Light ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.1f, 0.1f, 0.1f, 1.0f);

	/* ImGui's light preset draws the plot background as a 13% blue tint, which
	   leaves a plot inside a white window looking like a separate washed panel.
	   A plain white keeps the plot and the window reading as one surface. */
	if (GuiHelper::currentTheme == GuiHelper::Theme::Light)
	{
		ImPlot::GetStyle().Colors[ImPlotCol_FrameBg] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		ImPlot::GetStyle().Colors[ImPlotCol_PlotBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
	}

	logger->info("Interface theme applied: {}", GuiHelper::themeToName(GuiHelper::currentTheme));
}

/* Lays the panels out when there is nothing to restore.
 *
 * ImGui docks a window only if the layout file holds an entry for it, and the
 * entry is the title the window was opened with. A first run, a file the user
 * removed and a language that has not been started before all arrive with no
 * entry for these windows, and without this the panels would come up floating
 * at their default size on top of the dock space instead of filling it.
 *
 * The titles come from the table, so the names match the ones the drawing code
 * opens the windows with whichever language is in force. The saved layout of
 * one language is not the saved layout of another, which is why the check is
 * for a name rather than for the file: a language that has been used before
 * gets its own arrangement back, untouched.
 */
void Gui::ensureDockLayout(ImGuiID dockspaceId)
{
	if (dockLayoutChecked)
		return;

	dockLayoutChecked = true;

	/* The names carry their English after an id marker, so the layout file
	   keys every pane by something the language cannot move. That makes the
	   question "has a layout been saved for these panes?" answerable the same
	   way in either language.
	   
	   An entry on its own does not answer it, though. A file can carry one
	   left behind by a build that named its windows differently, and such an
	   entry describes a pane standing loose at whatever size the application
	   happened to start with - reading that as a layout would leave the panes
	   floating and skip the default below. What is asked instead is whether
	   the file actually puts one of them in a dock, which is the part a
	   default layout is there to supply. */
	bool anyPaneIsDocked = false;

	for (const char* pane : {"Var Viewer", "Trace Viewer", "Plots", "Trace Plots"})
	{
		ImGuiWindowSettings* settings = ImGui::FindWindowSettings(ImHashStr(I18n::windowTitle(pane).c_str()));

		if (settings != nullptr && settings->DockId != 0)
			anyPaneIsDocked = true;
	}

	if (anyPaneIsDocked)
		return;

	ImGui::DockBuilderRemoveNode(dockspaceId);
	ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);

	const ImVec2 size = ImGui::GetMainViewport()->WorkSize;
	ImGui::DockBuilderSetNodeSize(dockspaceId, size);

	ImGuiID settingsSide = 0;
	ImGuiID plotsSide = 0;
	ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.27f, &settingsSide, &plotsSide);

	/* The variable side and the trace side live in the same node and are
	   switched between with the tabs above it, and so do their plots. */
	ImGui::DockBuilderDockWindow(I18n::windowTitle("Var Viewer").c_str(), settingsSide);
	ImGui::DockBuilderDockWindow(I18n::windowTitle("Trace Viewer").c_str(), settingsSide);
	ImGui::DockBuilderDockWindow(I18n::windowTitle("Plots").c_str(), plotsSide);
	ImGui::DockBuilderDockWindow(I18n::windowTitle("Trace Plots").c_str(), plotsSide);

	ImGui::DockBuilderFinish(dockspaceId);
}

void Gui::applyLanguage()
{
	GlobalConfig::Settings& settings = globalConfig->getSettings();

	I18n::currentLanguage = I18n::languageFromName(settings.language);

	logger->info("Interface language applied: {}", I18n::languageToName(I18n::currentLanguage));
}

/* The fonts the Chinese interface is drawn with, in the order they are tried.
 *
 * Microsoft YaHei is the one Windows itself uses for interface text, so it is
 * the one the rest of the screen is already set in. The others are there for
 * machines where it was removed - a stripped down Windows image, or a Linux
 * install, where the Noto and WenQuanYi families are the usual answer.
 *
 * A missing font is not fatal. The interface comes up with the Latin glyphs it
 * would have had anyway, and every Chinese word draws as an empty box, which
 * says what went wrong more clearly than a dialog would. */
static const char* chineseFontCandidates[] = {
	"C:/Windows/Fonts/msyh.ttc",
	"C:/Windows/Fonts/msyhbd.ttc",
	"C:/Windows/Fonts/Deng.ttf",
	"C:/Windows/Fonts/simhei.ttf",
	"C:/Windows/Fonts/simsun.ttc",
	"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
	"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
	"/usr/share/fonts/truetype/arphic/uming.ttc",
};

void Gui::setupFonts()
{
	ImGuiIO& io = ImGui::GetIO();

	/* Cleared rather than added to, so that the function can be called again
	   for a language switch and produce the same atlas it produced the first
	   time. */
	io.Fonts->Clear();

	ImFontConfig cfg;
	cfg.SizePixels = 13.0f * GuiHelper::contentScale;

	/* The embedded font carries Latin. It goes in first, which is what makes
	   it the one the merged font below fills in the gaps of. */
	io.Fonts->AddFontDefault(&cfg);

	if (I18n::currentLanguage == I18n::Language::Chinese)
	{
		/* Handing FreeType the whole CJK block would bake several thousand
		   glyphs nobody looks at. The set that is actually needed is the set of
		   characters in the string table, so it is read from there - which
		   also means it cannot drift away from the table the way a hand
		   written list would. */
		ImFontGlyphRangesBuilder builder;
		builder.AddText(I18n::allText().c_str());

		/* A builder that was given nothing produces a list holding only its
		   terminator, which reads as "no glyphs at all" and would leave even
		   the Latin missing. */
		builder.AddRanges(io.Fonts->GetGlyphRangesDefault());

		/* Outlives this call on purpose: the atlas keeps the pointer and reads
		   the ranges when it is built, which happens later, on the first
		   frame. */
		static ImVector<ImWchar> ranges;
		builder.BuildRanges(&ranges);

		ImFontConfig merged;
		merged.MergeMode = true;
		merged.SizePixels = cfg.SizePixels;

		for (const char* path : chineseFontCandidates)
		{
			if (!std::filesystem::exists(path))
				continue;

			if (io.Fonts->AddFontFromFileTTF(path, cfg.SizePixels, &merged, ranges.Data) != nullptr)
			{
				logger->info("Chinese font loaded: {}", path);
				break;
			}

			logger->warn("Chinese font could not be read: {}", path);
		}

		/* Reported rather than raised: the interface is still usable without
		   it, and the reader can see for themselves which glyphs are missing. */
		if (io.Fonts->Fonts.Size < 2)
			logger->error("No Chinese font was found, so Chinese text will not be shown: {}", chineseFontCandidates[0]);
	}

	io.FontGlobalScale = globalConfig->getSettings().fontScale;
}

void Gui::rebuildFonts()
{
	/* The texture is built from the atlas, so the old one is dropped first.
	   It is made again from the atlas this builds, which is why the two calls
	   sit on either side of it rather than both after. */
	ImGui_ImplOpenGL3_DestroyFontsTexture();
	setupFonts();
	ImGui_ImplOpenGL3_CreateFontsTexture();
}

void Gui::drawPreferencesWindow()
{
	if (showPreferencesWindow)
	{
		themeIndex = static_cast<int>(GuiHelper::themeFromName(globalConfig->getSettings().theme));
		languageIndex = static_cast<int>(I18n::languageFromName(globalConfig->getSettings().language));
		ImGui::OpenPopup(I18n::windowTitle("Preferences").c_str());
	}

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(500 * GuiHelper::contentScale, 700 * GuiHelper::contentScale));
	if (ImGui::BeginPopupModal(I18n::windowTitle("Preferences").c_str(), &showPreferencesWindow, 0))
	{
		ImGuiIO& io = ImGui::GetIO();
		GlobalConfig::Settings& settings = globalConfig->getSettings();

		ImGui::TextUnformatted(I18n::tr("Appearance"));

		ImGui::SetNextItemWidth(200 * GuiHelper::contentScale);

		if (ImGui::Combo(I18n::tr("Theme##theme"), &themeIndex, "Dark\0Light\0"))
		{
			settings.theme = GuiHelper::themeToName(static_cast<GuiHelper::Theme>(themeIndex));
			logger->info("Interface theme set to {}", settings.theme);
			applyTheme();
		}

		ImGui::TextDisabled(I18n::tr("Applies to the whole interface, including the plots."));

		ImGui::SetNextItemWidth(200 * GuiHelper::contentScale);

		/* Each language is named in itself rather than translated into the one
		   in force, so that a reader who has landed in a language they do not
		   read can still find their way back out. */
		const char* languageNames[] = {I18n::tr("English"), I18n::tr("Chinese")};

		if (ImGui::Combo(I18n::tr("Language##language"), &languageIndex, languageNames, IM_ARRAYSIZE(languageNames)))
		{
			settings.language = I18n::languageToName(static_cast<I18n::Language>(languageIndex));
			applyLanguage();

			/* Read on the next frame, before anything is drawn with it: the
			   dialog runs where ImGui will not let the font atlas be rebuilt. */
			fontRebuildPending = true;
		}

		ImGui::TextDisabled(I18n::tr("Applies to the whole interface. The panels keep their places."));

		ImGui::Separator();

		ImGui::DragFloat(I18n::tr("font size"), &io.FontGlobalScale, 0.005f, 0.8f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		settings.fontScale = io.FontGlobalScale;

		ImGui::Separator();
		ImGui::TextUnformatted(I18n::tr("Project"));

		ImGui::Checkbox(I18n::tr("Autosave project"), &settings.autoSaveEnabled);
		ImGui::BeginDisabled(!settings.autoSaveEnabled);

		int interval = static_cast<int>(settings.autoSaveIntervalSeconds);
		ImGui::SetNextItemWidth(200 * GuiHelper::contentScale);

		if (ImGui::DragInt(I18n::tr("interval [s]"), &interval, 1.0f, 10, 3600, "%d", ImGuiSliderFlags_AlwaysClamp))
			settings.autoSaveIntervalSeconds = static_cast<uint32_t>(interval);

		ImGui::EndDisabled();
		ImGui::TextDisabled(I18n::tr("Saves the project file when it is modified and has a path."));

		ImGui::Separator();
		ImGui::TextUnformatted(I18n::tr("API server"));

		const bool mcpWasEnabled = settings.mcpEnabled;
		const uint16_t mcpWasPort = settings.mcpPreferredPort;

		ImGui::Checkbox(I18n::tr("Enable MCP server##mcp"), &settings.mcpEnabled);
		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("A local HTTP endpoint that exposes the project, the variable table and the acquisition as tools. "
								   "The server listens on the loopback interface only. It is off until it is switched on here."));

		ImGui::BeginDisabled(!settings.mcpEnabled);

		int mcpPort = static_cast<int>(settings.mcpPreferredPort);
		ImGui::SetNextItemWidth(200 * GuiHelper::contentScale);

		if (ImGui::DragInt(I18n::tr("port##mcp"), &mcpPort, 1.0f, 1024, 65535, "%d", ImGuiSliderFlags_AlwaysClamp))
			settings.mcpPreferredPort = static_cast<uint16_t>(mcpPort);

		ImGui::EndDisabled();
		ImGui::TextDisabled(I18n::tr("If the port is taken, the next free one is used."));

		ImGui::Checkbox(I18n::tr("Allow API writes to the target"), &settings.apiWritesEnabled);
		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("Without this, set_variable_value is refused. Each variable can carry its own min/max range on top."));

		if (mcpServer == nullptr)
			ImGui::TextDisabled(I18n::tr("Not available"));
		else if (mcpServer->isRunning())
			ImGui::Text(I18n::tr("Listening on %s (%zu tools)"), mcpServer->getEndpoint().c_str(), mcpServer->getToolCount());
		else if (settings.mcpEnabled)
			ImGui::TextColored(GuiHelper::redLight(), I18n::tr("Not listening: %s"), mcpServer->getLastError().c_str());
		else
			ImGui::TextDisabled(I18n::tr("Disabled"));

		/* The port is bound at start, so a change is applied right away instead of
		   waiting for the next launch. */
		if (settings.mcpEnabled != mcpWasEnabled || settings.mcpPreferredPort != mcpWasPort)
			applyMcpSettings();

		ImGui::Separator();
		drawPlotExportSettings();

		const float buttonHeight = 25.0f * GuiHelper::contentScale;
		ImGui::SetCursorPos(ImVec2(0, ImGui::GetWindowSize().y - buttonHeight / 2.0f - ImGui::GetFrameHeightWithSpacing()));
		if (ImGui::Button(I18n::tr("Done"), ImVec2(-1, buttonHeight)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			showPreferencesWindow = false;
			globalConfig->save();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void Gui::askShouldSaveOnExit(bool shouldOpenPopup)
{
	if (shouldOpenPopup)
		ImGui::OpenPopup(I18n::windowTitle("Save?").c_str());

	auto onYes = [&]()
	{
		done = true;
		if (!saveProject())
			saveProjectAs();
	};

	auto onNo = [&]()
	{ done = true; };
	auto onCancel = [&]()
	{ done = false; };

	if (variableHandler->isEmpty() && projectElfPath.empty() && shouldOpenPopup)
		done = true;

	GuiHelper::showQuestionBox("Save?", I18n::tr("Do you want to save the current config?\n"), onYes, onNo, onCancel);
}

void Gui::newProject()
{
	variableHandler->clear();
	plotHandler->removeAllPlots();
	tracePlotHandler->removeAllPlots();
	traceDataHandler->traceVars.clear();
	traceDataHandler->initPlots();
	plotGroupHandler->removeAllGroups();
	writePlanner->clear();
	projectElfPath = "";
	projectConfigPath = "";
	projectHandler->reset();
}

void Gui::askShouldSaveOnNew(bool shouldOpenPopup)
{
	auto onNo = [&]()
	{
		newProject();
	};

	if (variableHandler->isEmpty() && projectElfPath.empty() && shouldOpenPopup)
		onNo();
	else if (shouldOpenPopup)
		ImGui::OpenPopup(I18n::windowTitle("SaveOnNew?").c_str());

	auto onYes = [&]()
	{
		if (!saveProject())
			saveProjectAs();
		onNo();
	};

	GuiHelper::showQuestionBox("SaveOnNew?", I18n::tr("Do you want to save the current config?\n"), onYes, onNo, []() {});
}

bool Gui::saveProject()
{
	if (projectConfigPath.empty())
		return false;

	if (!projectHandler->save(projectConfigPath, collectProjectData()))
	{
		popup.show(I18n::tr("Error"), I18n::tr("Project could not be saved!"), 3.0f);
		return false;
	}

	globalConfig->addRecentProject(projectConfigPath);
	logger->info("Project config path: {}", projectConfigPath);

	return true;
}

bool Gui::saveProjectAs()
{
	const std::string extension = std::string(".") + ProjectHandler::fileExtension;

	std::string path = fileHandler->saveFile({{"Project files", ProjectHandler::fileExtension}, {"All files", "*"}});

	if (path.empty())
		return false;

	/* The system dialog only enforces the filter, so a name typed without an
	   extension has to be completed here. An explicit extension is kept. */
	if (std::filesystem::path(path).extension().empty())
		path += extension;

	/* The elf path is relative to the project file, so moving the project to
	   another directory has to re-anchor it. */
	const std::string rebasedElfPath = rebaseElfPath(path);

	ProjectData data = collectProjectData();
	data.elfPath = rebasedElfPath;

	if (!projectHandler->save(path, data))
	{
		popup.show(I18n::tr("Error"), I18n::tr("Project could not be saved!"), 3.0f);
		return false;
	}

	projectElfPath = rebasedElfPath;
	projectConfigPath = path;

	globalConfig->addRecentProject(projectConfigPath);
	globalConfig->save();

	logger->info("Project config path: {}", projectConfigPath);

	return true;
}

bool Gui::openProject(std::string externalPath)
{
	std::string path = "";

	if (externalPath.empty())
		path = fileHandler->openFile({{"Project files", ProjectHandler::fileExtension},
									  {"Legacy projects", ProjectHandler::legacyFileExtension},
									  {"All files", "*"}});
	else
		path = externalPath;

	if (path.empty())
		return false;

	/* Projects written before the JSON format are still readable, they are
	   converted on the next save. */
	if (!ProjectHandler::isJsonProjectFile(path))
		return importLegacyProject(path);

	ProjectData data;
	const ProjectHandler::OpenResult result = projectHandler->open(path, data);

	if (result != ProjectHandler::OpenResult::Ok)
	{
		popup.show(I18n::tr("Error"), I18n::tr(ProjectHandler::resultToString(result).c_str()), 3.0f);
		return false;
	}

	projectConfigPath = path;
	globalConfig->addRecentProject(projectConfigPath);
	globalConfig->save();

	applyProjectData(data);
	selectProbeDevices();

	/* The server comes up again on the loaded model, which is done on the next
	   frame because this can be reached from a tool call. */
	mcpRestartPending = true;

	logger->info("Project config path: {}", projectConfigPath);

	return true;
}

bool Gui::importLegacyProject(const std::string& path)
{
	/* The legacy reader owns the whole load sequence, including the variable and
	   plot model, so the handlers are cleared first for the same reason they are
	   cleared before a JSON load. */
	configHandler->changeConfigFile(path);
	variableHandler->clear();
	plotHandler->removeAllPlots();
	tracePlotHandler->removeAllPlots();
	traceDataHandler->traceVars.clear();
	plotGroupHandler->removeAllGroups();

	if (!configHandler->readConfigFile(projectElfPath))
	{
		popup.show(I18n::tr("Error"), I18n::tr("Legacy project could not be read!"), 3.0f);
		return false;
	}

	/* The legacy file is left untouched. The next save writes a project file next
	   to it, which keeps the relative elf path in the same directory valid and
	   makes the old and the new format easy to compare. */
	const std::filesystem::path legacyPath(path);
	projectConfigPath = (legacyPath.parent_path() / (legacyPath.stem().string() + "." + ProjectHandler::fileExtension)).string();

	/* Nothing was written yet, so the buffer counts as modified. */
	projectHandler->reset();

	globalConfig->addRecentProject(path);
	globalConfig->save();

	selectProbeDevices();

	mcpRestartPending = true;

	logger->info("Imported legacy project {}, save target is {}", path, projectConfigPath);
	popup.show(I18n::tr("Info"), I18n::tr("Legacy project imported. Use Save to store it as .mcvproj."), 4.0f);

	return true;
}

void Gui::applyProjectData(const ProjectData& data)
{
	projectElfPath = data.elfPath;

	ViewerDataHandler::Settings viewerSettings = viewerDataHandler->getSettings();
	viewerSettings.sampleFrequencyHz = data.viewer.sampleFrequencyHz;
	viewerSettings.maxPoints = data.viewer.maxPoints;
	viewerSettings.maxViewportPoints = data.viewer.maxViewportPoints;
	viewerSettings.refreshAddressesOnElfChange = data.viewer.refreshAddressesOnElfChange;
	viewerSettings.stopAcqusitionOnElfChange = data.viewer.stopAcquisitionOnElfChange;
	viewerSettings.shouldLog = data.viewer.loggingEnabled;
	viewerSettings.logFilePath = data.viewer.logDirectory;
	viewerSettings.gdbCommand = data.viewer.gdbCommand;
	viewerSettings.elfParser = data.viewer.elfParser;
	viewerSettings.ofd2000Command = data.viewer.ofd2000Command;
	viewerDataHandler->setSettings(viewerSettings);

	/* A project that was written before the parser became a setting names the
	   default one, and its symbol file may belong to a target that needs the
	   other one. Looking at the file is the only way to notice, and it costs
	   one read of a header. */
	inspectElfTarget(GuiHelper::convertProjectPathToAbsolute(&projectElfPath, &projectConfigPath));

	IDebugProbe::DebugProbeSettings debugProbeSettings = viewerDataHandler->getProbeSettings();
	debugProbeSettings.debugProbe = data.viewer.probe.type;
	debugProbeSettings.serialNumber = data.viewer.probe.serialNumber;
	debugProbeSettings.device = data.viewer.probe.targetName;
	debugProbeSettings.mode = static_cast<IDebugProbe::Mode>(data.viewer.probe.mode);
	debugProbeSettings.speedkHz = data.viewer.probe.speedKHz;
	debugProbeSettings.baudrate = data.viewer.probe.baudrate;
	debugProbeSettings.keepConnection = data.viewer.probe.keepConnection;
	viewerDataHandler->setProbeSettings(debugProbeSettings);

	TraceDataHandler::Settings traceSettings = traceDataHandler->getSettings();
	traceSettings.coreFrequency = data.trace.coreFrequency;
	traceSettings.tracePrescaler = data.trace.tracePrescaler;
	traceSettings.maxPoints = data.trace.maxPoints;
	traceSettings.maxViewportPointsPercent = data.trace.maxViewportPointsPercent;
	traceSettings.triggerChannel = data.trace.triggerChannel;
	traceSettings.triggerLevel = data.trace.triggerLevel;
	traceSettings.shouldReset = data.trace.shouldReset;
	traceSettings.timeout = data.trace.timeout;
	traceSettings.shouldLog = data.trace.loggingEnabled;
	traceSettings.logFilePath = data.trace.logDirectory;
	traceDataHandler->setSettings(traceSettings);

	ITraceProbe::TraceProbeSettings traceProbeSettings = traceDataHandler->getProbeSettings();
	traceProbeSettings.debugProbe = data.trace.probe.type;
	traceProbeSettings.serialNumber = data.trace.probe.serialNumber;
	traceProbeSettings.device = data.trace.probe.targetName;
	traceProbeSettings.speedkHz = data.trace.probe.speedKHz;
	traceProbeSettings.keepConnection = data.trace.probe.keepConnection;
	traceDataHandler->setProbeSettings(traceProbeSettings);

	applyTracePlots(data.tracePlots);

	/* The plans follow the project, because what they write to is one of its
	   variables. */
	writePlanner->clear();

	for (const ProjectWritePlan& entry : data.writePlans)
	{
		if (!writePlanner->addPlan(entry.name, entry.variable))
			continue;

		std::vector<WritePlanner::Step> steps;
		steps.reserve(entry.steps.size());

		for (const ProjectWriteStep& step : entry.steps)
			steps.push_back(WritePlanner::Step{step.time, step.value});

		writePlanner->setSteps(entry.name, steps);
	}

	logger->info("Loaded write plans: {}", writePlanner->size());
}

std::string Gui::rebaseElfPath(const std::string& newProjectPath)
{
	if (projectElfPath.empty())
		return "";

	std::error_code errorCode;

	std::filesystem::path absoluteElf(projectElfPath);

	if (absoluteElf.is_relative())
		absoluteElf = std::filesystem::absolute(std::filesystem::path(projectConfigPath).parent_path() / absoluteElf, errorCode);

	if (errorCode)
		return projectElfPath;

	errorCode.clear();

	const std::filesystem::path newDirectory = std::filesystem::path(newProjectPath).parent_path();
	const std::filesystem::path relativeElf = std::filesystem::relative(absoluteElf, newDirectory, errorCode);

	/* A path on another volume has no relative form, so the absolute one is kept. */
	if (errorCode || relativeElf.empty())
		return absoluteElf.string();

	return relativeElf.string();
}

void Gui::selectProbeDevices()
{
	/* The probe type comes from the project, so the object behind the handler has
	   to follow it. The device list is rebuilt on the next polling cycle. */
	devicesList.clear();

	debugProbeDevice = probeForType(viewerDataHandler->getProbeSettings().debugProbe);
	viewerDataHandler->setDebugProbe(debugProbeDevice);

	if (traceDataHandler->getProbeSettings().debugProbe == 1)
		traceProbeDevice = jlinkTraceProbe;
	else
		traceProbeDevice = stlinkTraceProbe;

	traceDataHandler->setDebugProbe(traceProbeDevice);
}

std::shared_ptr<IDebugProbe> Gui::probeForType(uint32_t type) const
{
	switch (type)
	{
		case IDebugProbe::Probe::Jlink:
			return jlinkProbe;

		case IDebugProbe::Probe::Serial:
			return serialProbe;

		default:
			return stlinkProbe;
	}
}

bool Gui::openElfFile()
{
	std::string path = fileHandler->openFile({{"Elf files", "elf,axf,out"}, {"All files", "*"}});

	if (path.find(" ") != std::string::npos && parserNeedsPathWithoutSpaces())
	{
		acqusitionErrorPopup.show(I18n::tr("Error!"), I18n::tr("Selected path contains spaces!"), 2.0f);
		projectElfPath = "";
		return false;
	}

	if (path != "")
	{
		std::filesystem::path relPath = std::filesystem::relative(path, std::filesystem::path(projectConfigPath).parent_path());
		if (relPath != "")
			projectElfPath = relPath.string();
		else
			projectElfPath = path;

		logger->info("Project elf file path: {}", projectElfPath);

		inspectElfTarget(path);

		return true;
	}
	return false;
}

bool Gui::parserNeedsPathWithoutSpaces() const
{
	/* Only the GDB parser builds a command line out of the path without quoting
	   it; the C2000 one quotes it, and the DWARF one reads the file in process
	   and builds no command line at all. */
	return viewerDataHandler->getSettings().elfParser == IElfParser::nameOf(IElfParser::Type::Gdb);
}

void Gui::inspectElfTarget(const std::string& path)
{
	if (detectElfTarget(path) != ElfTarget::C2000)
		return;

	/* Nothing to suggest when the parser in use already counts in the target's
	   own units. */
	if (viewerDataHandler->getSettings().elfParser == IElfParser::nameOf(IElfParser::Type::TiOfd))
		return;

	c2000SuggestionPath = path;
	showC2000ParserSuggestion = true;
}

void Gui::drawC2000ParserSuggestion()
{
	/* The popup is opened once per suggestion rather than on every frame: ImGui
	   closes a modal on Escape, and opening it again immediately would leave the
	   user with a box that cannot be dismissed. */
	if (!showC2000ParserSuggestion)
	{
		c2000SuggestionOpened = false;
		return;
	}

	if (!c2000SuggestionOpened)
	{
		ImGui::OpenPopup(I18n::windowTitle("C2000 Target Detected").c_str());
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		c2000SuggestionOpened = true;
	}

	if (ImGui::BeginPopupModal(I18n::windowTitle("C2000 Target Detected").c_str(), &showC2000ParserSuggestion, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted(I18n::tr("The selected file appears to be a TI C2000 target binary."));
		ImGui::Dummy(ImVec2(0, 4));
		ImGui::TextUnformatted(I18n::tr("It is highly recommended to switch to the C2000 parser"));
		ImGui::Dummy(ImVec2(0, 4));
		ImGui::TextDisabled("%s", c2000SuggestionPath.c_str());
		ImGui::Dummy(ImVec2(0, 8));

		if (ImGui::Button(I18n::tr("Switch to C2000 parser (recommended)"), ImVec2(-1, 25 * GuiHelper::contentScale)))
		{
			ViewerDataHandler::Settings settings = viewerDataHandler->getSettings();
			settings.elfParser = IElfParser::nameOf(IElfParser::Type::TiOfd);
			viewerDataHandler->setSettings(settings);

			logger->info("Switched to the C2000 ELF parser for '{}'", c2000SuggestionPath);

			showC2000ParserSuggestion = false;
			ImGui::CloseCurrentPopup();
		}

		if (ImGui::Button(I18n::tr("Keep current parser"), ImVec2(-1, 25 * GuiHelper::contentScale)))
		{
			showC2000ParserSuggestion = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}

bool Gui::openLogDirectory(std::string& logDirectory)
{
	std::string path = fileHandler->openDirectory({});

	if (path != "")
	{
		logDirectory = path;
		logger->info("Log directory: {}", path);
		return true;
	}
	return false;
}

void Gui::checkShortcuts()
{
	const ImGuiIO& io = ImGui::GetIO();

	/* Plots are worth saving precisely while an acquisition is filling them, so
	   this one is not held back by the gate below.
	   The second argument turns off key repeat: with it on, holding the two
	   keys down would save the same plots again on every repeat, and each pass
	   would overwrite the files the previous one had just numbered. */
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P, false))
		requestPlotImages();

	/* The File menu disables these entries while an acquisition is running,
	   because loading a project replaces the whole variable and plot model. */
	const bool active = !(viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN || traceDataHandler->getStateImmediate() == DataHandlerBase::State::RUN);

	if (!io.KeyCtrl || !active)
		return;

	if (ImGui::IsKeyPressed(ImGuiKey_O))
		openProject();
	else if (ImGui::IsKeyPressed(ImGuiKey_N))
		askShouldSaveOnNew(true);
	else if (ImGui::IsKeyPressed(ImGuiKey_S))
	{
		bool wasSaved = saveProject();

		if (!wasSaved)
			wasSaved = saveProjectAs();

		if (wasSaved)
			popup.show(I18n::tr("Info"), I18n::tr("Saving successful!"), 0.65f);
	}
}

void Gui::showChangeFormatPopup(const char* text, Plot& plt, const std::string& name)
{
	int format = static_cast<int>(plt.getSeriesDisplayFormat(name));

	if (plt.getSeries(name)->var->getType() == Variable::Type::F32)
		return;

	if (ImGui::BeginPopupContextItem(name.c_str()))
	{
		if (ImGui::RadioButton(I18n::tr("dec"), &format, 0))
			ImGui::CloseCurrentPopup();
		if (ImGui::RadioButton(I18n::tr("hex"), &format, 1))
			ImGui::CloseCurrentPopup();
		if (ImGui::RadioButton(I18n::tr("bin"), &format, 2))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	plt.setSeriesDisplayFormat(name, static_cast<Plot::displayFormat>(format));
}

/* ------------------------------------------------------------------ API server */

void Gui::setupMcpServer()
{
	syncMcpElfParser();

	mcpBridge = std::make_unique<mcp::Bridge>();
	mcpContext = std::make_unique<McpContext>();

	mcpContext->variableHandler = variableHandler;
	mcpContext->plotHandler = plotHandler;
	mcpContext->tracePlotHandler = tracePlotHandler;
	mcpContext->plotGroupHandler = plotGroupHandler;
	mcpContext->viewerDataHandler = viewerDataHandler;
	mcpContext->traceDataHandler = traceDataHandler;
	mcpContext->globalConfig = globalConfig;
	mcpContext->projectHandler = projectHandler;
	mcpContext->logger = logger;
	mcpContext->mtx = mtx;

	/* The recorder is owned by the window, and the tools work through the same
	   object, so a setting changed over the API is the one the window shows. */
	mcpContext->recorderHandler = recorderHandler.get();
	mcpContext->syncRecorderSymbols = [this]()
	{ return syncRecorderSymbols(); };

	/* Same reasoning for the programmer: a flash started over the API and one
	   started from the toolbar are the same run, and the panel shows it either
	   way. */
	mcpContext->flasher = flasher.get();

	mcpContext->openProject = [this](const std::string& path)
	{ return openProject(path); };

	mcpContext->saveProjectAs = [this](const std::string& path)
	{
		/* The API is given the target file, so the picker behind saveProjectAs()
		   cannot be used. The extension handling and the elf rebase are repeated
		   here so a path without an extension still lands on a project file. */
		std::string target = path;

		if (std::filesystem::path(target).extension().empty())
			target += std::string(".") + ProjectHandler::fileExtension;

		const std::string rebasedElfPath = rebaseElfPath(target);

		ProjectData data = collectProjectData();
		data.elfPath = rebasedElfPath;

		if (!projectHandler->save(target, data))
		{
			popup.show(I18n::tr("Error"), I18n::tr("Project could not be saved!"), 3.0f);
			return false;
		}

		projectElfPath = rebasedElfPath;
		projectConfigPath = target;

		globalConfig->addRecentProject(projectConfigPath);
		globalConfig->save();

		logger->info("Project config path: {}", projectConfigPath);

		return true;
	};

	mcpContext->saveProject = [this]()
	{ return saveProject(); };

	mcpContext->newProject = [this]()
	{ newProject(); };

	mcpContext->setElfPath = [this](const std::string& path, bool relative)
	{ return setElfPathFromApi(path, relative); };

	mcpContext->refreshVariableAddresses = [this](const std::string& elfPath)
	{ return refreshVariableAddressesFromElf(elfPath); };

	mcpContext->selectProbeDevice = [this]()
	{ selectProbeDevices(); };

	mcpContext->getProjectPath = [this]()
	{ return projectConfigPath; };

	mcpContext->getElfPath = [this]()
	{ return GuiHelper::convertProjectPathToAbsolute(&projectElfPath, &projectConfigPath); };

	mcpContext->getApplicationVersion = []()
	{
		return std::to_string(VARIABLE_TRACE_VERSION_MAJOR) + "." +
			   std::to_string(VARIABLE_TRACE_VERSION_MINOR) + "." +
			   std::to_string(VARIABLE_TRACE_VERSION_REVISION);
	};

	mcpContext->getServerPort = [this]()
	{ return mcpServer == nullptr ? static_cast<uint16_t>(0) : mcpServer->getPort(); };

	mcpContext->getApiWritesEnabled = [this]()
	{ return globalConfig->getSettings().apiWritesEnabled; };

	mcpServer = std::make_unique<mcp::Server>(mcpContext.get(), mcpBridge.get(), logger);

	applyMcpSettings();
}

void Gui::applyMcpSettings()
{
	if (mcpServer == nullptr)
		return;

	const GlobalConfig::Settings& settings = globalConfig->getSettings();

	mcpServer->restart(settings.mcpEnabled, settings.mcpPreferredPort);

	if (!settings.mcpEnabled)
	{
		logger->info("API server is disabled");
		return;
	}

	if (mcpServer->isRunning())
		logger->info("API server available at {} with {} tools", mcpServer->getEndpoint(), mcpServer->getToolCount());
	else
		popup.show(I18n::tr("API server"), (std::string(I18n::tr("The server could not be started: ")) + mcpServer->getLastError()).c_str(), 5.0f);
}

bool Gui::setElfPathFromApi(const std::string& path, bool relative)
{
	/* The GDB command line carries the path without quoting it, so a path with a
	   space would split into several arguments. The other parsers quote it or
	   read the file themselves, so the restriction is theirs alone. */
	if (path.find(' ') != std::string::npos && parserNeedsPathWithoutSpaces())
	{
		acqusitionErrorPopup.show(I18n::tr("Error!"), I18n::tr("Selected path contains spaces!"), 2.0f);
		return false;
	}

	if (!relative)
	{
		projectElfPath = path;
		logger->info("Project elf file path: {}", projectElfPath);
		inspectElfTarget(path);
		return true;
	}

	std::error_code errorCode;
	std::filesystem::path absoluteElf = std::filesystem::absolute(path, errorCode);

	if (errorCode)
		return false;

	const std::filesystem::path directory = std::filesystem::path(projectConfigPath).parent_path();
	std::error_code rebaseError;
	const std::filesystem::path rebased = std::filesystem::relative(absoluteElf, directory, rebaseError);

	/* A path on another volume has no relative form, so the absolute one is kept,
	   the same way the file picker handles it. */
	projectElfPath = (rebaseError || rebased.empty()) ? absoluteElf.string() : rebased.string();

	logger->info("Project elf file path: {}", projectElfPath);

	inspectElfTarget(absoluteElf.string());

	return true;
}

void Gui::syncMcpElfParser()
{
	const std::string name = viewerDataHandler->getSettings().elfParser;

	if (mcpElfParser != nullptr && name == mcpElfParserName)
	{
		ElfParserFactory::applySettings(*mcpElfParser, viewerDataHandler->getSettings().gdbCommand,
										viewerDataHandler->getSettings().ofd2000Command);
		return;
	}

	mcpElfParser = ElfParserFactory::create(name, variableHandler, logger);
	mcpElfParserName = mcpElfParser->getName();
}

bool Gui::refreshVariableAddressesFromElf(const std::string& elfPath)
{
	syncMcpElfParser();

	if (mcpElfParser == nullptr)
		return false;

	/* The update spawns a child process that runs for seconds, so the
	   application mutex is not held across it: the mutex also guards the per
	   frame buffer copy, and holding it would stall the interface for that
	   whole time. The variable table performs its refresh the same way. */
	const bool updated = mcpElfParser->updateVariableMap(elfPath);

	if (updated && variableTable != nullptr)
		variableTable->markElfRefreshed();

	return updated;
}
