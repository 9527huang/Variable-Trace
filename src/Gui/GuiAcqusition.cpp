#include "Gui.hpp"

#include "LibDwarfParser.hpp"
#include "TiOfdParser.hpp"

static constexpr size_t alignment = 30;

void Gui::drawAcqusitionSettingsScope(ActiveViewType type)
{
	const bool isVariableViewer = (type == ActiveViewType::VarViewer);

	ImGui::Dummy(ImVec2(-1, 5));

	ImGui::PushStyleColor(ImGuiCol_Text, isVariableViewer ? GuiHelper::greenLight : GuiHelper::orangeLight);
	GuiHelper::drawCenteredText(isVariableViewer ? "These settings belong to the VARIABLE VIEWER" : "These settings belong to the TRACE VIEWER");
	GuiHelper::drawCenteredText(isVariableViewer ? "Debug probe, SWD speed and Target name here drive the RAM variable acquisition."
	                                            : "Debug probe, SWD speed and Target name here drive the SWO trace acquisition only.");
	ImGui::PopStyleColor();

	ImGui::Dummy(ImVec2(-1, 5));
	ImGui::Separator();
}

void Gui::acqusitionSettingsViewer()
{
	ImGui::Dummy(ImVec2(-1, 5));
	GuiHelper::drawCenteredText("General");
	ImGui::Separator();

	GuiHelper::drawTextAlignedToSize("*.elf file:", alignment);
	ImGui::SameLine();
	ImGui::InputText("##", &projectElfPath, 0, NULL, NULL);
	ImGui::SameLine();
	if (ImGui::Button("...", ImVec2(35 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
		openElfFile();

	ViewerDataHandler::Settings settings = viewerDataHandler->getSettings();

	GuiHelper::drawTextAlignedToSize("Refresh vars on *.elf change:", alignment);
	ImGui::SameLine();
	ImGui::Checkbox("##refresh", &settings.refreshAddressesOnElfChange);

	GuiHelper::drawTextAlignedToSize("Stop on *.elf change:", alignment);
	ImGui::SameLine();
	ImGui::Checkbox("##stop", &settings.stopAcqusitionOnElfChange);

	GuiHelper::drawTextAlignedToSize("Sampling [Hz]:", alignment);
	ImGui::SameLine();
	ImGui::InputScalar("##sample", ImGuiDataType_U32, &settings.sampleFrequencyHz, NULL, NULL, "%u");
	ImGui::SameLine();
	ImGui::HelpMarker("Maximum sampling frequency. Depending on the used debug probe it can be reached or not.");
	settings.sampleFrequencyHz = std::clamp(settings.sampleFrequencyHz, ViewerDataHandler::minSamplinFrequencyHz, ViewerDataHandler::maxSamplinFrequencyHz);

	const uint32_t minPoints = 100;
	const uint32_t maxPoints = 20000;
	GuiHelper::drawTextAlignedToSize("Max points:", alignment);
	ImGui::SameLine();
	ImGui::InputScalar("##maxPoints", ImGuiDataType_U32, &settings.maxPoints, NULL, NULL, "%u");
	ImGui::SameLine();
	ImGui::HelpMarker("Max points used for a single series after which the oldest points will be overwritten.");
	settings.maxPoints = std::clamp(settings.maxPoints, minPoints, maxPoints);

	GuiHelper::drawTextAlignedToSize("Max view points:", alignment);
	ImGui::SameLine();
	ImGui::InputScalar("##maxViewportPoints", ImGuiDataType_U32, &settings.maxViewportPoints, NULL, NULL, "%u");
	ImGui::SameLine();
	ImGui::HelpMarker("Max points used for a single series that will be shown in the viewport without scroling.");
	settings.maxViewportPoints = std::clamp(settings.maxViewportPoints, minPoints, settings.maxPoints);

	drawDebugProbes();
	drawLoggingSettings(plotHandler, settings);
	drawElfSettings(settings);
	viewerDataHandler->setSettings(settings);
}

void Gui::drawDebugProbes()
{
	static bool shouldListDevices = false;
	static int SNptr = 0;
	bool modified = false;

	ImGui::PushID("DebugProbes");
	ImGui::Dummy(ImVec2(-1, 5));
	GuiHelper::drawCenteredText("Debug Probe");
	ImGui::SameLine();
	ImGui::HelpMarker("Select the debug probe type and the serial number of the probe to unlock the START button.");
	ImGui::Separator();

	GuiHelper::drawTextAlignedToSize("Debug probe:", alignment);
	ImGui::SameLine();

	const char* debugProbes[] = {"STLINK", "JLINK", "SERIAL"};
	IDebugProbe::DebugProbeSettings probeSettings = viewerDataHandler->getProbeSettings();
	int32_t debugProbe = static_cast<int32_t>(probeSettings.debugProbe);

	if (ImGui::Combo("##debugProbe", &debugProbe, debugProbes, IM_ARRAYSIZE(debugProbes)))
	{
		probeSettings.debugProbe = static_cast<uint32_t>(debugProbe);
		modified = true;

		/* The list holds serial numbers for the two hardware probes and port
		   names for the serial one, so it is rebuilt whenever the type moves. */
		debugProbeDevice = probeForType(probeSettings.debugProbe);
		shouldListDevices = true;
		SNptr = 0;
	}

	const bool isSerial = probeSettings.debugProbe == IDebugProbe::Probe::Serial;

	GuiHelper::drawTextAlignedToSize(isSerial ? "Serial port:" : "Debug probe S/N:", alignment);
	ImGui::SameLine();

	if (ImGui::Combo("##debugProbeSN", &SNptr, devicesList))
	{
		probeSettings.serialNumber = devicesList[SNptr];
		modified = true;
	}

	ImGui::SameLine();

	if (ImGui::Button("@", ImVec2(35 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)) || shouldListDevices || devicesList.empty())
	{
		devicesList = debugProbeDevice->getConnectedDevices();
		if (!devicesList.empty())
		{
			probeSettings.serialNumber = devicesList[0];
			modified = true;
		}
		shouldListDevices = false;
	}

	if (isSerial)
	{
		/* The port carries the driver protocol and nothing else, so the only
		   setting left is the line speed. */
		GuiHelper::drawTextAlignedToSize("Baudrate:", alignment);
		ImGui::SameLine();

		if (ImGui::InputScalar("##baudrate", ImGuiDataType_U32, &probeSettings.baudrate, NULL, NULL, "%u"))
			modified = true;

		ImGui::SameLine();
		ImGui::HelpMarker("Line speed of the serial port. It has to match the baud rate the target firmware configures for its UART.");
		probeSettings.mode = IDebugProbe::Mode::NORMAL;
	}
	else
	{
		GuiHelper::drawTextAlignedToSize("SWD speed [kHz]:", alignment);
		ImGui::SameLine();

		if (ImGui::InputScalar("##speed", ImGuiDataType_U32, &probeSettings.speedkHz, NULL, NULL, "%u"))
			modified = true;
	}

	if (probeSettings.debugProbe == IDebugProbe::Probe::Jlink)
	{
		GuiHelper::drawTextAlignedToSize("Target name:", alignment);
		ImGui::SameLine();

		if (ImGui::InputText("##device", &probeSettings.device, 0, NULL, NULL))
			modified = true;

		ImGui::SameLine();
		if (ImGui::Button("...", ImVec2(35 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
		{
			std::string selectedTargetName = debugProbeDevice->getTargetName();
			if (selectedTargetName.empty())
				acqusitionErrorPopup.show("Target name", "No device was picked in the J-Link dialog, the current Target name was kept.", 2.0f);
			else
			{
				probeSettings.device = selectedTargetName;
				modified = true;
			}
		}

		GuiHelper::drawTextAlignedToSize("Mode:", alignment);
		ImGui::SameLine();

		const char* probeModes[] = {"NORMAL", "HSS"};
		int32_t probeMode = probeSettings.mode;

		if (ImGui::Combo("##mode", &probeMode, probeModes, IM_ARRAYSIZE(probeModes)))
		{
			probeSettings.mode = static_cast<IDebugProbe::Mode>(probeMode);
			modified = true;
		}

		ImGui::SameLine();
		ImGui::HelpMarker("Select normal or high speed sampling (HSS) mode");

		if (probeSettings.device.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight);
			GuiHelper::drawCenteredText("Target name is empty, the J-Link cannot connect. Enter the device name, e.g. STM32F103C8.");
			ImGui::PopStyleColor();
		}
	}
	else if (!isSerial)
		probeSettings.mode = IDebugProbe::Mode::NORMAL;

	if (devicesList.empty())
		devicesList.push_back(noDevices);

	if (modified)
	{
		viewerDataHandler->setProbeSettings(probeSettings);
		viewerDataHandler->setDebugProbe(debugProbeDevice);
	}
	ImGui::PopID();
}

template <typename Settings>
void Gui::drawLoggingSettings(PlotHandler* handler, Settings& settings)
{
	ImGui::PushID("logging");
	ImGui::Dummy(ImVec2(-1, 5));
	GuiHelper::drawCenteredText("Logging");
	ImGui::SameLine();
	ImGui::HelpMarker("Log all registered variables values to a selected log directory. File is created automatically and overwritten on each start.");
	ImGui::Separator();

	/* CSV streamer */
	GuiHelper::drawTextAlignedToSize("Log to file:", alignment);
	ImGui::SameLine();
	ImGui::Checkbox("##logging", &settings.shouldLog);

	ImGui::BeginDisabled(!settings.shouldLog);

	GuiHelper::drawTextAlignedToSize("Logfile directory:", alignment);
	ImGui::SameLine();
	ImGui::InputText("##", &settings.logFilePath, 0, NULL, NULL);
	ImGui::SameLine();
	if (ImGui::Button("...", ImVec2(35 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
		openLogDirectory(settings.logFilePath);

	ImGui::EndDisabled();
	ImGui::PopID();
}

void Gui::drawElfSettings(ViewerDataHandler::Settings& settings)
{
	ImGui::PushID("advanced");
	ImGui::Dummy(ImVec2(-1, 5));
	GuiHelper::drawCenteredText("Advanced");
	ImGui::Separator();

	/* The parser comes first, because which program has to be named below it
	   depends on which one is chosen. */
	const std::vector<IElfParser::Type> types = ElfParserFactory::availableTypes();

	std::vector<std::string> labels;
	labels.reserve(types.size());

	int current = 0;

	for (size_t index = 0; index < types.size(); index++)
	{
		labels.push_back(ElfParserFactory::labelOf(types[index]));

		if (IElfParser::nameOf(types[index]) == settings.elfParser)
			current = static_cast<int>(index);
	}

	std::vector<const char*> labelPointers;
	labelPointers.reserve(labels.size());

	for (const std::string& label : labels)
		labelPointers.push_back(label.c_str());

	GuiHelper::drawTextAlignedToSize("*.elf parser:", alignment);
	ImGui::SameLine();

	if (ImGui::Combo("##elfParser", &current, labelPointers.data(), static_cast<int>(labelPointers.size())))
		settings.elfParser = IElfParser::nameOf(types[static_cast<size_t>(current)]);

	ImGui::SameLine();
	ImGui::HelpMarker("Choose the ELF parser.");

	ImGui::SameLine();
	ImGui::HelpMarker("GDB: reliable, default. DWARF: faster variable address updates (beta). C2000: for TI C2000 targets, uses ofd2000.");

	const IElfParser::Type selected = types[static_cast<size_t>(current)];

	if (selected == IElfParser::Type::Gdb)
	{
		GuiHelper::drawTextAlignedToSize("GDB command:", alignment);
		ImGui::SameLine();
		ImGui::InputText("##gdb", &settings.gdbCommand, 0, NULL, NULL);
		ImGui::SameLine();
		ImGui::HelpMarker("Change to other GDB variant if needed. Given program will be run to update variables addresses.");
	}
	else if (selected == IElfParser::Type::TiOfd)
	{
		GuiHelper::drawTextAlignedToSize("ofd2000 path:", alignment);
		ImGui::SameLine();
		ImGui::InputText("##ofd2000Path", &settings.ofd2000Command, 0, NULL, NULL);
		ImGui::SameLine();

		if (ImGui::Button("...", ImVec2(35 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
		{
			const std::string picked = fileHandler->openFile({{"ofd2000 executable", "exe"}, {"All files", "*"}});

			if (!picked.empty())
				settings.ofd2000Command = picked;
		}

		ImGui::SameLine();
		ImGui::HelpMarker("Auto-detected from the TI C2000 code generation tools / Code Composer Studio install. Override here if not found automatically.");

		GuiHelper::drawTextAlignedToSize("ofd2000 status:", alignment);
		ImGui::SameLine();

		if (TiOfdParser::toolExists(settings.ofd2000Command))
			ImGui::TextUnformatted("OK");
		else
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight);
			ImGui::TextUnformatted("NOT FOUND. Please set the ofd2000 path manually");
			ImGui::PopStyleColor();
		}
	}
	else if (!LibDwarfParser::isCompiledIn())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight);
		GuiHelper::drawCenteredText("This build was compiled without the DWARF library, so this parser cannot read a file.");
		ImGui::PopStyleColor();
	}

	ImGui::PopID();
}

void Gui::acqusitionSettingsTrace()
{
	TraceDataHandler::Settings settings = traceDataHandler->getSettings();

	ImGui::Dummy(ImVec2(-1, 5));
	GuiHelper::drawCenteredText("General");
	ImGui::Separator();

	GuiHelper::drawTextAlignedToSize("Max points:", alignment);
	ImGui::SameLine();
	ImGui::InputScalar("##maxPoints", ImGuiDataType_U32, &settings.maxPoints, NULL, NULL, "%u");
	ImGui::SameLine();
	ImGui::HelpMarker("Max points used for a single series after which the oldest points will be overwritten.");
	settings.maxPoints = std::clamp(settings.maxPoints, static_cast<uint32_t>(100), static_cast<uint32_t>(20000));

	GuiHelper::drawTextAlignedToSize("Viewport width [%%]:", alignment);
	ImGui::SameLine();
	ImGui::InputScalar("##maxViewportPoints", ImGuiDataType_U32, &settings.maxViewportPointsPercent, NULL, NULL, "%u");
	ImGui::SameLine();
	ImGui::HelpMarker("The percentage of trace time visible during collect. Expressed in percent since the sample period is not constant.");
	settings.maxViewportPointsPercent = std::clamp(settings.maxViewportPointsPercent, static_cast<uint32_t>(1), static_cast<uint32_t>(100));

	GuiHelper::drawTextAlignedToSize("Timeout [s]:", alignment);
	ImGui::SameLine();
	ImGui::InputScalar("##timeout", ImGuiDataType_U32, &settings.timeout, NULL, NULL, "%u");
	ImGui::SameLine();
	ImGui::HelpMarker("Timeout is the period after which trace will be stopped due to no trace data being received.");
	settings.timeout = std::clamp(settings.timeout, static_cast<uint32_t>(1), static_cast<uint32_t>(999999));

	drawTraceProbes();
	drawLoggingSettings(tracePlotHandler, settings);
	traceDataHandler->setSettings(settings);
}

void Gui::drawTraceProbes()
{
	static bool shouldListDevices = false;
	static int SNptr = 0;
	bool modified = false;

	ImGui::PushID("DebugProbes");
	ImGui::Dummy(ImVec2(-1, 5));
	GuiHelper::drawCenteredText("Debug Probe");
	ImGui::SameLine();
	ImGui::HelpMarker("Select the debug probe type and the serial number of the probe to unlock the START button.");
	ImGui::Separator();

	GuiHelper::drawTextAlignedToSize("Debug probe:", alignment);
	ImGui::SameLine();

	const char* debugProbes[] = {"STLINK", "JLINK"};
	ITraceProbe::TraceProbeSettings probeSettings = traceDataHandler->getProbeSettings();
	int32_t debugProbe = probeSettings.debugProbe;

	if (ImGui::Combo("##debugProbe", &debugProbe, debugProbes, IM_ARRAYSIZE(debugProbes)))
	{
		probeSettings.debugProbe = debugProbe;
		modified = true;

		if (probeSettings.debugProbe == 1)
		{
			traceProbeDevice = jlinkTraceProbe;
			shouldListDevices = true;
		}
		else
		{
			traceProbeDevice = stlinkTraceProbe;
			shouldListDevices = true;
		}
		SNptr = 0;
	}
	GuiHelper::drawTextAlignedToSize("Debug probe S/N:", alignment);
	ImGui::SameLine();

	if (ImGui::Combo("##debugProbeSN", &SNptr, devicesList))
	{
		probeSettings.serialNumber = devicesList[SNptr];
		modified = true;
	}

	ImGui::SameLine();

	if (ImGui::Button("@", ImVec2(35 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)) || shouldListDevices || devicesList.empty())
	{
		devicesList = traceProbeDevice->getConnectedDevices();
		if (!devicesList.empty())
		{
			probeSettings.serialNumber = devicesList[0];
			modified = true;
		}
		shouldListDevices = false;
	}

	GuiHelper::drawTextAlignedToSize("SWD speed [kHz]:", alignment);
	ImGui::SameLine();

	if (ImGui::InputScalar("##speed", ImGuiDataType_U32, &probeSettings.speedkHz, NULL, NULL, "%u"))
		modified = true;

	if (probeSettings.debugProbe == 1)
	{
		GuiHelper::drawTextAlignedToSize("Target name:", alignment);
		ImGui::SameLine();

		if (ImGui::InputText("##device", &probeSettings.device, 0, NULL, NULL))
			modified = true;

		ImGui::SameLine();
		if (ImGui::Button("...", ImVec2(35 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
		{
			std::string selectedTargetName = traceProbeDevice->getTargetName();
			if (selectedTargetName.empty())
				acqusitionErrorPopup.show("Target name", "No device was picked in the J-Link dialog, the current Target name was kept.", 2.0f);
			else
			{
				probeSettings.device = selectedTargetName;
				modified = true;
			}
		}

		if (probeSettings.device.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight);
			GuiHelper::drawCenteredText("Target name is empty, the J-Link cannot connect. Enter the device name, e.g. STM32F103C8.");
			ImGui::PopStyleColor();
		}
	}

	if (devicesList.empty())
		devicesList.push_back(noDevices);

	if (modified)
	{
		traceDataHandler->setProbeSettings(probeSettings);
		traceDataHandler->setDebugProbe(traceProbeDevice);
	}
	ImGui::PopID();
}