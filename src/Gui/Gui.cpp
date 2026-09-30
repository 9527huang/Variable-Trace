#include "Gui.hpp"

#include <imgui.h>
#include <unistd.h>

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

#include "McpBridge.hpp"
#include "McpContext.hpp"
#include "McpServer.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

Gui::Gui(PlotHandler* plotHandler, VariableHandler* variableHandler, ConfigHandler* configHandler, ProjectHandler* projectHandler, GlobalConfig* globalConfig, PlotGroupHandler* plotGroupHandler, IFileHandler* fileHandler, PlotHandler* tracePlotHandler, ViewerDataHandler* viewerDataHandler, TraceDataHandler* traceDataHandler, std::atomic<bool>& done, std::mutex* mtx, spdlog::logger* logger, std::string& projectPath) : plotHandler(plotHandler), variableHandler(variableHandler), configHandler(configHandler), projectHandler(projectHandler), globalConfig(globalConfig), plotGroupHandler(plotGroupHandler), fileHandler(fileHandler), tracePlotHandler(tracePlotHandler), viewerDataHandler(viewerDataHandler), traceDataHandler(traceDataHandler), done(done), mtx(mtx), logger(logger)
{
	lastAutoSaveTime = std::chrono::steady_clock::now();
	plotEditWindow = std::make_shared<PlotEditWindow>(plotHandler, plotGroupHandler, variableHandler);
	plotsTree = std::make_shared<PlotsTree>(viewerDataHandler, plotHandler, plotGroupHandler, variableHandler, plotEditWindow, fileHandler, logger);
	variableTable = std::make_shared<VariableTableWindow>(viewerDataHandler, plotHandler, variableHandler, &projectElfPath, &projectConfigPath, logger);

	variableHandler->renameCallback = [&](std::string oldName, std::string newName)
	{
		for (std::shared_ptr<Plot> plt : *this->plotHandler)
			plt->renameSeries(oldName, newName);
	};

	/* The recorder reads and writes whichever probe the viewer acquired, so it
	   needs nothing beyond the logger to exist. */
	recorderHandler = std::make_unique<recorder::RecorderHandler>(logger);

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

	ImFontConfig cfg;
	cfg.SizePixels = 13.0f * GuiHelper::contentScale;

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.Fonts->AddFontDefault(&cfg);
	io.FontGlobalScale = globalConfig->getSettings().fontScale;

	ImGui::StyleColorsDark();
	ImPlot::StyleColorsDark();

	ImGui::GetStyle().ScaleAllSizes(GuiHelper::contentScale);
	ImGui::GetStyle().Colors[ImGuiCol_PopupBg] = ImVec4(0.1f, 0.1f, 0.1f, 1.0f);

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

		if (glfwGetWindowAttrib(window, GLFW_FOCUSED) || (traceDataHandler->getState() == DataHandlerBase::State::RUN) || (viewerDataHandler->getState() == DataHandlerBase::State::RUN))
			glfwSwapInterval(1);
		else
			glfwSwapInterval(4);

		glfwSetWindowTitle(window, (std::string("Variable-Trace - ") + projectConfigPath).c_str());
		glfwPollEvents();
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();

		ImGui::NewFrame();
		ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());

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
		drawRecorderWindow();
		drawFlashOutputWindow();
		drawFlashingSettingsWindow();
		drawWritePlannerWindow();
		drawC2000ParserSuggestion();

		if (ImGui::Begin("Trace Viewer"))
		{
			activeView = ActiveViewType::TraceViewer;
			drawAcqusitionSettingsWindow(activeView);
			ImGui::SetNextWindowClass(&window_class);
			if (ImGui::Begin("Trace Plots"))
				drawPlotsSwo();
			ImGui::End();
			drawStartButton(traceDataHandler);
			drawSettingsSwo();
			drawIndicatorsSwo();
			drawPlotsTreeSwo();
		}
		ImGui::End();

		if (ImGui::Begin("Var Viewer"))
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
			if (ImGui::Begin("Plots"))
				drawPlots();
			ImGui::End();
		}
		ImGui::End();

		popup.handle();
		flashPopup.handle();

		// Rendering
		ImGui::Render();
		int display_w, display_h;
		glfwGetFramebufferSize(window, &display_w, &display_h);
		glViewport(0, 0, display_w, display_h);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

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

	bool active = !(viewerDataHandler->getState() == DataHandlerBase::State::RUN || traceDataHandler->getState() == DataHandlerBase::State::RUN);

	if (ImGui::BeginMenu("File"))
	{
		/* Loading a project replaces every variable and plot, so it is only
		   offered while no acquisition is running. */
		if (ImGui::MenuItem("New", "Ctrl+N", false, active))
			shouldSaveOnNew = true;

		if (ImGui::MenuItem("Open", "Ctrl+O", false, active))
			openProject();

		drawOpenRecentMenu();

		if (ImGui::MenuItem("Save", "Ctrl+S", false, (!projectConfigPath.empty())))
			saveProject();

		if (ImGui::MenuItem("Save As.."))
			saveProjectAs();

		if (ImGui::MenuItem("Quit"))
			shouldSaveOnClose = true;

		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Options"))
	{
		ImGui::MenuItem("Acquisition settings...", NULL, &showAcqusitionSettingsWindow, active);

		/* The programmer and the firmware file belong to the machine rather than
		   to the project, which is why they are reached from here and not from
		   the project file dialog. */
		ImGui::MenuItem("Flashing", NULL, &showFlashingSettingsWindow, true);

		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Window"))
	{
		ImGui::MenuItem("Recorder", NULL, &showRecorderWindow, active);
		ImGui::MenuItem("Flash Output", NULL, &showFlashOutputWindow, true);
		ImGui::MenuItem("Write Planner", NULL, &showWritePlannerWindow, active);
		ImGui::MenuItem("Preferences", NULL, &showPreferencesWindow, active);
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Help"))
	{
		ImGui::MenuItem("About", NULL, &showAboutWindow, active);
		ImGui::EndMenu();
	}

	if (activeView == ActiveViewType::VarViewer)
	{
		ImGui::SetCursorPosX((ImGui::GetWindowSize().x - 210 * GuiHelper::contentScale));
		GuiHelper::drawDescriptionWithNumber("sampling: ", viewerDataHandler->getAverageSamplingFrequency(), " Hz", 2);
	}

	ImGui::EndMainMenuBar();
	askShouldSaveOnExit(shouldSaveOnClose);
	askShouldSaveOnNew(shouldSaveOnNew);
}

void Gui::drawOpenRecentMenu()
{
	GlobalConfig::Settings& settings = globalConfig->getSettings();

	if (!ImGui::BeginMenu("Open Recent", !settings.recentProjects.empty()))
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

	if (ImGui::MenuItem("Clear list"))
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

	DataHandlerBase::State state = activeDataHandler->getState();

	if (state == DataHandlerBase::State::RUN)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, GuiHelper::green);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GuiHelper::greenLight);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, GuiHelper::greenLightDim);
	}
	else if (state == DataHandlerBase::State::STOP)
	{
		if (activeDataHandler->getLastReaderError() != "")
		{
			ImGui::PushStyleColor(ImGuiCol_Button, GuiHelper::red);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GuiHelper::redLight);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, GuiHelper::redLightDim);
		}
		else
		{
			ImGui::PushStyleColor(ImGuiCol_Button, GuiHelper::orange);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GuiHelper::orangeLight);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, GuiHelper::orangeLightDim);
		}
	}

	if ((ImGui::Button((viewerStateMap.at(state) + " " + activeDataHandler->getLastReaderError()).c_str(), ImVec2(-1, 50 * GuiHelper::contentScale)) ||
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
		ImGui::OpenPopup("Acqusition Settings");

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(950 * GuiHelper::contentScale, 600 * GuiHelper::contentScale));
	if (ImGui::BeginPopupModal("Acqusition Settings", &showAcqusitionSettingsWindow, 0))
	{
		drawAcqusitionSettingsScope(type);

		if (type == ActiveViewType::VarViewer)
			acqusitionSettingsViewer();
		else if (type == ActiveViewType::TraceViewer)
			acqusitionSettingsTrace();

		acqusitionErrorPopup.handle();

		const float buttonHeight = 25.0f * GuiHelper::contentScale;
		ImGui::SetCursorPos(ImVec2(0, ImGui::GetWindowSize().y - buttonHeight / 2.0f - ImGui::GetFrameHeightWithSpacing()));

		if (ImGui::Button("Done", ImVec2(-1, buttonHeight)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			showAcqusitionSettingsWindow = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}

void Gui::drawPreferencesWindow()
{
	if (showPreferencesWindow)
		ImGui::OpenPopup("Preferences");

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(500 * GuiHelper::contentScale, 520 * GuiHelper::contentScale));
	if (ImGui::BeginPopupModal("Preferences", &showPreferencesWindow, 0))
	{
		ImGuiIO& io = ImGui::GetIO();
		GlobalConfig::Settings& settings = globalConfig->getSettings();

		ImGui::DragFloat("font size", &io.FontGlobalScale, 0.005f, 0.8f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		settings.fontScale = io.FontGlobalScale;

		ImGui::Separator();
		ImGui::TextUnformatted("Project");

		ImGui::Checkbox("Autosave project", &settings.autoSaveEnabled);
		ImGui::BeginDisabled(!settings.autoSaveEnabled);

		int interval = static_cast<int>(settings.autoSaveIntervalSeconds);
		ImGui::SetNextItemWidth(200 * GuiHelper::contentScale);

		if (ImGui::DragInt("interval [s]", &interval, 1.0f, 10, 3600, "%d", ImGuiSliderFlags_AlwaysClamp))
			settings.autoSaveIntervalSeconds = static_cast<uint32_t>(interval);

		ImGui::EndDisabled();
		ImGui::TextDisabled("Saves the project file when it is modified and has a path.");

		ImGui::Separator();
		ImGui::TextUnformatted("API server");

		const bool mcpWasEnabled = settings.mcpEnabled;
		const uint16_t mcpWasPort = settings.mcpPreferredPort;

		ImGui::Checkbox("Enable MCP server##mcp", &settings.mcpEnabled);
		ImGui::SameLine();
		ImGui::HelpMarker("A local HTTP endpoint that exposes the project, the variable table and the acquisition as tools. "
						  "The server listens on the loopback interface only. It is off until it is switched on here.");

		ImGui::BeginDisabled(!settings.mcpEnabled);

		int mcpPort = static_cast<int>(settings.mcpPreferredPort);
		ImGui::SetNextItemWidth(200 * GuiHelper::contentScale);

		if (ImGui::DragInt("port##mcp", &mcpPort, 1.0f, 1024, 65535, "%d", ImGuiSliderFlags_AlwaysClamp))
			settings.mcpPreferredPort = static_cast<uint16_t>(mcpPort);

		ImGui::EndDisabled();
		ImGui::TextDisabled("If the port is taken, the next free one is used.");

		ImGui::Checkbox("Allow API writes to the target", &settings.apiWritesEnabled);
		ImGui::SameLine();
		ImGui::HelpMarker("Without this, set_variable_value is refused. Each variable can carry its own min/max range on top.");

		if (mcpServer == nullptr)
			ImGui::TextDisabled("Not available");
		else if (mcpServer->isRunning())
			ImGui::Text("Listening on %s (%zu tools)", mcpServer->getEndpoint().c_str(), mcpServer->getToolCount());
		else if (settings.mcpEnabled)
			ImGui::TextColored(GuiHelper::redLight, "Not listening: %s", mcpServer->getLastError().c_str());
		else
			ImGui::TextDisabled("Disabled");

		/* The port is bound at start, so a change is applied right away instead of
		   waiting for the next launch. */
		if (settings.mcpEnabled != mcpWasEnabled || settings.mcpPreferredPort != mcpWasPort)
			applyMcpSettings();

		const float buttonHeight = 25.0f * GuiHelper::contentScale;
		ImGui::SetCursorPos(ImVec2(0, ImGui::GetWindowSize().y - buttonHeight / 2.0f - ImGui::GetFrameHeightWithSpacing()));
		if (ImGui::Button("Done", ImVec2(-1, buttonHeight)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
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
		ImGui::OpenPopup("Save?");

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

	GuiHelper::showQuestionBox("Save?", "Do you want to save the current config?\n", onYes, onNo, onCancel);
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
		ImGui::OpenPopup("SaveOnNew?");

	auto onYes = [&]()
	{
		if (!saveProject())
			saveProjectAs();
		onNo();
	};

	GuiHelper::showQuestionBox("SaveOnNew?", "Do you want to save the current config?\n", onYes, onNo, []() {});
}

bool Gui::saveProject()
{
	if (projectConfigPath.empty())
		return false;

	if (!projectHandler->save(projectConfigPath, collectProjectData()))
	{
		popup.show("Error", "Project could not be saved!", 3.0f);
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
		popup.show("Error", "Project could not be saved!", 3.0f);
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
		popup.show("Error", ProjectHandler::resultToString(result).c_str(), 3.0f);
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
		popup.show("Error", "Legacy project could not be read!", 3.0f);
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
	popup.show("Info", "Legacy project imported. Use Save to store it as .mcvproj.", 4.0f);

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
		acqusitionErrorPopup.show("Error!", "Selected path contains spaces!", 2.0f);
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
		ImGui::OpenPopup("C2000 Target Detected");
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		c2000SuggestionOpened = true;
	}

	if (ImGui::BeginPopupModal("C2000 Target Detected", &showC2000ParserSuggestion, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted("The selected file appears to be a TI C2000 target binary.");
		ImGui::Dummy(ImVec2(0, 4));
		ImGui::TextUnformatted("It is highly recommended to switch to the C2000 parser");
		ImGui::Dummy(ImVec2(0, 4));
		ImGui::TextDisabled("%s", c2000SuggestionPath.c_str());
		ImGui::Dummy(ImVec2(0, 8));

		if (ImGui::Button("Switch to C2000 parser (recommended)", ImVec2(-1, 25 * GuiHelper::contentScale)))
		{
			ViewerDataHandler::Settings settings = viewerDataHandler->getSettings();
			settings.elfParser = IElfParser::nameOf(IElfParser::Type::TiOfd);
			viewerDataHandler->setSettings(settings);

			logger->info("Switched to the C2000 ELF parser for '{}'", c2000SuggestionPath);

			showC2000ParserSuggestion = false;
			ImGui::CloseCurrentPopup();
		}

		if (ImGui::Button("Keep current parser", ImVec2(-1, 25 * GuiHelper::contentScale)))
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

	/* The File menu disables these entries while an acquisition is running,
	   because loading a project replaces the whole variable and plot model. */
	const bool active = !(viewerDataHandler->getState() == DataHandlerBase::State::RUN || traceDataHandler->getState() == DataHandlerBase::State::RUN);

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
			popup.show("Info", "Saving successful!", 0.65f);
	}
}

void Gui::showChangeFormatPopup(const char* text, Plot& plt, const std::string& name)
{
	int format = static_cast<int>(plt.getSeriesDisplayFormat(name));

	if (plt.getSeries(name)->var->getType() == Variable::Type::F32)
		return;

	if (ImGui::BeginPopupContextItem(name.c_str()))
	{
		if (ImGui::RadioButton("dec", &format, 0))
			ImGui::CloseCurrentPopup();
		if (ImGui::RadioButton("hex", &format, 1))
			ImGui::CloseCurrentPopup();
		if (ImGui::RadioButton("bin", &format, 2))
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
			popup.show("Error", "Project could not be saved!", 3.0f);
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
		popup.show("API server", ("The server could not be started: " + mcpServer->getLastError()).c_str(), 5.0f);
}

bool Gui::setElfPathFromApi(const std::string& path, bool relative)
{
	/* The GDB command line carries the path without quoting it, so a path with a
	   space would split into several arguments. The other parsers quote it or
	   read the file themselves, so the restriction is theirs alone. */
	if (path.find(' ') != std::string::npos && parserNeedsPathWithoutSpaces())
	{
		acqusitionErrorPopup.show("Error!", "Selected path contains spaces!", 2.0f);
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
