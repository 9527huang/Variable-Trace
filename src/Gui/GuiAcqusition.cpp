#include "Gui.hpp"

#include "I18n.hpp"
#include "LibDwarfParser.hpp"
#include "TiOfdParser.hpp"

/* Wide enough for the longest label in this window, which is the one about
   refreshing on a change of the symbol file. */
static constexpr size_t alignment = 36;

/* Device names the J-Link takes for a core rather than for a part. They matter
   where a named device makes the probe halt the target on every start: the
   generic name is read without that halt, which is the difference between a
   reading that flows and one that stops the thing being measured.

   The field stays free text - picking one of these only fills it in, and a part
   number is still typed the way it always was. */
static const char* const jlinkGenericTargets[] = {"Cortex-M3", "Cortex-M4"};

/* The target name, as a combo: the box carries the current name and the list
   opens on a click anywhere on it, exactly the way the Mode list below
   behaves. Typing is the first row of that list rather than a field beside the
   box - a combo cannot hold text of its own, and a field next to one reads as
   two controls instead of one.

   It is drawn by both probe sections, which is why it lives out here. */
static void drawJlinkTargetName(std::string& targetName, bool& modified)
{
	GuiHelper::drawTextAlignedToSize(I18n::tr("Target name:"), alignment);
	ImGui::SameLine();

	if (ImGui::BeginCombo("##device", targetName.c_str()))
	{
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

		/* Typed whole, then closed with Enter: half a part number is not
		   something to hand the probe. */
		if (ImGui::InputTextWithHint("##typed", I18n::tr("type a name"), &targetName,
									 ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL))
		{
			modified = true;
			ImGui::CloseCurrentPopup();
		}

		ImGui::Separator();

		for (const char* target : jlinkGenericTargets)
		{
			if (ImGui::Selectable(target, targetName == target))
			{
				targetName = target;
				modified = true;
			}
		}

		ImGui::EndCombo();
	}

	ImGui::SameLine();
}

void Gui::drawAcqusitionSettingsScope(ActiveViewType type)
{
	const bool isVariableViewer = (type == ActiveViewType::VarViewer);

	ImGui::Dummy(ImVec2(-1, 5));

	ImGui::PushStyleColor(ImGuiCol_Text, isVariableViewer ? GuiHelper::greenLight() : GuiHelper::orangeLight());
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
	GuiHelper::drawCenteredText(I18n::tr("General"));
	ImGui::Separator();

	GuiHelper::drawTextAlignedToSize(I18n::tr("*.elf/*.axf file:"), alignment);
	ImGui::SameLine();
	ImGui::InputText("##", &projectElfPath, 0, NULL, NULL);
	ImGui::SameLine();
	if (ImGui::Button("...", ImVec2(35 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
		openElfFile();

	ViewerDataHandler::Settings settings = viewerDataHandler->getSettings();

	GuiHelper::drawTextAlignedToSize(I18n::tr("Refresh vars on *.elf/*.axf change:"), alignment);
	ImGui::SameLine();
	ImGui::Checkbox("##refresh", &settings.refreshAddressesOnElfChange);

	GuiHelper::drawTextAlignedToSize(I18n::tr("Stop on *.elf/*.axf change:"), alignment);
	ImGui::SameLine();
	ImGui::Checkbox("##stop", &settings.stopAcqusitionOnElfChange);

	GuiHelper::drawTextAlignedToSize(I18n::tr("Sampling [Hz]:"), alignment);
	ImGui::SameLine();
	ImGui::InputScalar("##sample", ImGuiDataType_U32, &settings.sampleFrequencyHz, NULL, NULL, "%u");
	ImGui::SameLine();
	ImGui::HelpMarker(I18n::tr("Maximum sampling frequency. Depending on the used debug probe it can be reached or not."));
	settings.sampleFrequencyHz = std::clamp(settings.sampleFrequencyHz, ViewerDataHandler::minSamplinFrequencyHz, ViewerDataHandler::maxSamplinFrequencyHz);

	const uint32_t minPoints = 100;

	/* How many samples a series may keep follows from the memory the buffers
	   of this group are allowed, and that follows from how many buffers there
	   are. So the ceiling is asked for rather than written down here, and the
	   arithmetic behind it is shown beside the field: a request that comes
	   back smaller than what was typed has to say why. */
	const uint32_t maxPoints = plotHandler->maxPointsForMemoryBudget();
	const size_t bufferCount = plotHandler->countBuffers();
	const uint64_t maxPointsBytes = static_cast<uint64_t>(maxPoints) * PlotHandler::bytesPerBufferedPoint * bufferCount;

	std::string maxPointsHelp = I18n::tr("Max points used for a single series after which the oldest points will be overwritten.");
	maxPointsHelp += "\n\n" + std::to_string(maxPoints) + " x " + std::to_string(bufferCount) + " x " +
					 std::to_string(PlotHandler::bytesPerBufferedPoint) + " B = " + std::to_string(maxPointsBytes >> 20) + " MB";

	GuiHelper::drawTextAlignedToSize(I18n::tr("Max points:"), alignment);
	ImGui::SameLine();
	ImGui::InputScalar("##maxPoints", ImGuiDataType_U32, &settings.maxPoints, NULL, NULL, "%u");
	ImGui::SameLine();
	ImGui::HelpMarker(maxPointsHelp.c_str());
	settings.maxPoints = std::clamp(settings.maxPoints, minPoints, maxPoints);

	GuiHelper::drawTextAlignedToSize(I18n::tr("Viewport width [ms]:"), alignment);
	ImGui::SameLine();
	ImGui::InputScalar("##viewportWidthMs", ImGuiDataType_U32, &settings.viewportWidthMs, NULL, NULL, "%u");
	ImGui::SameLine();
	ImGui::HelpMarker(I18n::tr("How much time the viewport shows while acquisition is running. What can be shown is bounded by the samples that are kept: Max points at the sampling frequency."));
	settings.viewportWidthMs = std::clamp(settings.viewportWidthMs, ViewerDataHandler::minViewportWidthMs, ViewerDataHandler::maxViewportWidthMs);

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
	GuiHelper::drawCenteredText(I18n::tr("Debug Probe"));
	ImGui::SameLine();
	ImGui::HelpMarker(I18n::tr("Select the debug probe type and the serial number of the probe to unlock the START button."));
	ImGui::Separator();

	GuiHelper::drawTextAlignedToSize(I18n::tr("Debug probe:"), alignment);
	ImGui::SameLine();

	IDebugProbe::DebugProbeSettings probeSettings = viewerDataHandler->getProbeSettings();
	int32_t debugProbe = static_cast<int32_t>(probeSettings.debugProbe);

	if (ImGui::Combo("##debugProbe", &debugProbe, IDebugProbe::probeNames, IDebugProbe::probeNameCount))
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
	const bool isGdbServer = probeSettings.debugProbe == IDebugProbe::Probe::GdbServer;

	/* A GDB server is not a device that can be listed: it is reached at an
	   address, which the branch below asks for. */
	if (!isGdbServer)
	{
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
	}

	if (isSerial)
	{
		/* The port carries the driver protocol and nothing else, so the only
		   setting left is the line speed. */
		GuiHelper::drawTextAlignedToSize(I18n::tr("Baudrate:"), alignment);
		ImGui::SameLine();

		if (ImGui::InputScalar("##baudrate", ImGuiDataType_U32, &probeSettings.baudrate, NULL, NULL, "%u"))
			modified = true;

		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("Line speed of the serial port. It has to match the baud rate the target firmware configures for its UART."));
		probeSettings.mode = IDebugProbe::Mode::NORMAL;
	}
	else if (isGdbServer)
	{
		/* Nothing here reaches the target on its own: a GDB server does the
		   reaching, and this says where it is. */
		probeSettings.mode = IDebugProbe::Mode::NORMAL;

		GuiHelper::drawTextAlignedToSize(I18n::tr("GDB server:"), alignment);
		ImGui::SameLine();

		if (ImGui::InputText("##gdbAddress", &probeSettings.ipAddress, 0, NULL, NULL))
			modified = true;

		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("Address of the machine running the GDB server. OpenOCD, and the servers shipped with probes, both speak the protocol this reads through."));

		GuiHelper::drawTextAlignedToSize(I18n::tr("Port:"), alignment);
		ImGui::SameLine();

		if (ImGui::InputScalar("##gdbPort", ImGuiDataType_U32, &probeSettings.port, NULL, NULL, "%u"))
			modified = true;

		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("Port the GDB server serves on. OpenOCD's default is 3333."));

		if (probeSettings.ipAddress.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight());
			GuiHelper::drawCenteredText(I18n::tr("No GDB server address, the acquisition cannot connect without one."));
			ImGui::PopStyleColor();
		}
	}
	else
	{
		GuiHelper::drawTextAlignedToSize(I18n::tr("SWD speed [kHz]:"), alignment);
		ImGui::SameLine();

		if (ImGui::InputScalar("##speed", ImGuiDataType_U32, &probeSettings.speedkHz, NULL, NULL, "%u"))
			modified = true;
	}

	if (probeSettings.debugProbe == IDebugProbe::Probe::Jlink)
	{
		drawJlinkTargetName(probeSettings.device, modified);

		if (ImGui::Button("...", ImVec2(35 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
		{
			std::string selectedTargetName = debugProbeDevice->getTargetName();
			if (selectedTargetName.empty())
				acqusitionErrorPopup.show(I18n::tr("Target name"), I18n::tr("No device was picked in the J-Link dialog, the current Target name was kept."), 2.0f);
			else
			{
				probeSettings.device = selectedTargetName;
				modified = true;
			}
		}

		GuiHelper::drawTextAlignedToSize(I18n::tr("Mode:"), alignment);
		ImGui::SameLine();

		const char* probeModes[] = {I18n::tr("NORMAL"), "HSS"};
		int32_t probeMode = probeSettings.mode;

		if (ImGui::Combo("##mode", &probeMode, probeModes, IM_ARRAYSIZE(probeModes)))
		{
			probeSettings.mode = static_cast<IDebugProbe::Mode>(probeMode);
			modified = true;
		}

		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("Select normal or high speed sampling (HSS) mode"));

		GuiHelper::drawTextAlignedToSize(I18n::tr("Keep connection:"), alignment);
		ImGui::SameLine();

		if (ImGui::Checkbox("##keepConnection", &probeSettings.keepConnection))
			modified = true;

		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("Keeps the connection to the probe open when a run ends. Closing it halts the target on some setups, and that halt is what the next run has to undo before it can read anything."));

		if (probeSettings.device.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight());
			GuiHelper::drawCenteredText(I18n::tr("Target name is empty, the J-Link cannot connect. Enter the device name, e.g. STM32F103C8."));
			ImGui::PopStyleColor();
		}

		GuiHelper::drawTextAlignedToSize(I18n::tr("Connection:"), alignment);
		ImGui::SameLine();

		const char* jlinkConnections[] = {"USB", "TCP/IP"};
		int32_t jlinkConnection = static_cast<int32_t>(probeSettings.connection);

		if (ImGui::Combo("##connection", &jlinkConnection, jlinkConnections, IM_ARRAYSIZE(jlinkConnections)))
		{
			probeSettings.connection = static_cast<IDebugProbe::Connection>(jlinkConnection);
			modified = true;
		}

		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("How the J-Link is reached: over USB, or over the network at the address below."));

		if (probeSettings.connection == IDebugProbe::Connection::Network)
		{
			GuiHelper::drawTextAlignedToSize(I18n::tr("IP address:"), alignment);
			ImGui::SameLine();

			if (ImGui::InputText("##jlinkIp", &probeSettings.ipAddress, 0, NULL, NULL))
				modified = true;

			ImGui::SameLine();
			ImGui::HelpMarker(I18n::tr("A probe reached this way is picked by its address instead of by its serial number."));
		}

		GuiHelper::drawTextAlignedToSize(I18n::tr("JLink script:"), alignment);
		ImGui::SameLine();

		if (ImGui::Checkbox("##scriptEnabled", &probeSettings.scriptEnabled))
			modified = true;

		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("Runs a J-Link script of your own before the target is connected - for targets that need a watchdog stopped or a reset in a particular order."));

		if (probeSettings.scriptEnabled)
		{
			GuiHelper::drawTextAlignedToSize(I18n::tr("script file:"), alignment);
			ImGui::SameLine();

			if (ImGui::InputText("##scriptPath", &probeSettings.scriptPath, 0, NULL, NULL))
				modified = true;

			ImGui::SameLine();

			if (ImGui::Button("...", ImVec2(35 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
			{
				const std::string picked = fileHandler->openFile({{"J-Link script", "JLinkScript"}, {"All files", "*"}});

				if (!picked.empty())
				{
					probeSettings.scriptPath = picked;
					modified = true;
				}
			}
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
	GuiHelper::drawCenteredText(I18n::tr("Logging"));
	ImGui::SameLine();
	ImGui::HelpMarker(I18n::tr("Log all registered variables values to a selected log directory. File is created automatically and overwritten on each start."));
	ImGui::Separator();

	/* CSV streamer */
	GuiHelper::drawTextAlignedToSize(I18n::tr("Log to file:"), alignment);
	ImGui::SameLine();
	ImGui::Checkbox("##logging", &settings.shouldLog);

	ImGui::BeginDisabled(!settings.shouldLog);

	GuiHelper::drawTextAlignedToSize(I18n::tr("Logfile directory:"), alignment);
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
	GuiHelper::drawCenteredText(I18n::tr("Advanced"));
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

	GuiHelper::drawTextAlignedToSize(I18n::tr("*.elf/*.axf parser:"), alignment);
	ImGui::SameLine();

	if (ImGui::Combo("##elfParser", &current, labelPointers.data(), static_cast<int>(labelPointers.size())))
		settings.elfParser = IElfParser::nameOf(types[static_cast<size_t>(current)]);

	ImGui::SameLine();
	ImGui::HelpMarker(I18n::tr("Choose the ELF parser."));

	ImGui::SameLine();
	ImGui::HelpMarker(I18n::tr("GDB: reliable, default. DWARF: faster variable address updates (beta). C2000: for TI C2000 targets, uses ofd2000."));

	const IElfParser::Type selected = types[static_cast<size_t>(current)];

	if (selected == IElfParser::Type::Gdb)
	{
		GuiHelper::drawTextAlignedToSize(I18n::tr("GDB command:"), alignment);
		ImGui::SameLine();
		ImGui::InputText("##gdb", &settings.gdbCommand, 0, NULL, NULL);
		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("Change to other GDB variant if needed. Given program will be run to update variables addresses."));
	}
	else if (selected == IElfParser::Type::TiOfd)
	{
		GuiHelper::drawTextAlignedToSize(I18n::tr("ofd2000 path:"), alignment);
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
		ImGui::HelpMarker(I18n::tr("Auto-detected from the TI C2000 code generation tools / Code Composer Studio install. Override here if not found automatically."));

		GuiHelper::drawTextAlignedToSize(I18n::tr("ofd2000 status:"), alignment);
		ImGui::SameLine();

		if (TiOfdParser::toolExists(settings.ofd2000Command))
			ImGui::TextUnformatted(I18n::tr("OK"));
		else
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight());
			ImGui::TextUnformatted(I18n::tr("NOT FOUND. Please set the ofd2000 path manually"));
			ImGui::PopStyleColor();
		}
	}
	else if (!LibDwarfParser::isCompiledIn())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight());
		GuiHelper::drawCenteredText(I18n::tr("This build was compiled without the DWARF library, so this parser cannot read a file."));
		ImGui::PopStyleColor();
	}

	ImGui::PopID();
}

void Gui::acqusitionSettingsTrace()
{
	TraceDataHandler::Settings settings = traceDataHandler->getSettings();

	ImGui::Dummy(ImVec2(-1, 5));
	GuiHelper::drawCenteredText(I18n::tr("General"));
	ImGui::Separator();

	/* The ceiling is the same rule as on the viewer side: what the buffers of
	   this handler may take from memory, divided by what one sample costs in
	   each of them. */
	const uint32_t minPoints = 100;
	const uint32_t maxPoints = tracePlotHandler->maxPointsForMemoryBudget();
	const size_t bufferCount = tracePlotHandler->countBuffers();
	const uint64_t maxPointsBytes = static_cast<uint64_t>(maxPoints) * PlotHandler::bytesPerBufferedPoint * bufferCount;

	std::string maxPointsHelp = I18n::tr("Max points used for a single series after which the oldest points will be overwritten.");
	maxPointsHelp += "\n\n" + std::to_string(maxPoints) + " x " + std::to_string(bufferCount) + " x " +
					 std::to_string(PlotHandler::bytesPerBufferedPoint) + " B = " + std::to_string(maxPointsBytes >> 20) + " MB";

	GuiHelper::drawTextAlignedToSize(I18n::tr("Max points:"), alignment);
	ImGui::SameLine();
	ImGui::InputScalar("##maxPoints", ImGuiDataType_U32, &settings.maxPoints, NULL, NULL, "%u");
	ImGui::SameLine();
	ImGui::HelpMarker(maxPointsHelp.c_str());
	settings.maxPoints = std::clamp(settings.maxPoints, minPoints, maxPoints);

	GuiHelper::drawTextAlignedToSize(I18n::tr("Viewport width [%%]:"), alignment);
	ImGui::SameLine();
	ImGui::InputScalar("##maxViewportPoints", ImGuiDataType_U32, &settings.maxViewportPointsPercent, NULL, NULL, "%u");
	ImGui::SameLine();
	ImGui::HelpMarker(I18n::tr("The percentage of trace time visible during collect. Expressed in percent since the sample period is not constant."));
	settings.maxViewportPointsPercent = std::clamp(settings.maxViewportPointsPercent, static_cast<uint32_t>(1), static_cast<uint32_t>(100));

	GuiHelper::drawTextAlignedToSize(I18n::tr("Timeout [s]:"), alignment);
	ImGui::SameLine();
	ImGui::InputScalar("##timeout", ImGuiDataType_U32, &settings.timeout, NULL, NULL, "%u");
	ImGui::SameLine();
	ImGui::HelpMarker(I18n::tr("Timeout is the period after which trace will be stopped due to no trace data being received."));
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
	GuiHelper::drawCenteredText(I18n::tr("Debug Probe"));
	ImGui::SameLine();
	ImGui::HelpMarker(I18n::tr("Select the debug probe type and the serial number of the probe to unlock the START button."));
	ImGui::Separator();

	GuiHelper::drawTextAlignedToSize(I18n::tr("Debug probe:"), alignment);
	ImGui::SameLine();

	/* The trace stream comes out of two probes only, and neither is a GDB
	   server, so this list belongs to the trace side. A project file naming
	   anything else falls back to the first entry - the same one the
	   acquisition falls back to - rather than indexing past the list. */
	const char* debugProbes[] = {"STLINK", "JLINK"};
	ITraceProbe::TraceProbeSettings probeSettings = traceDataHandler->getProbeSettings();
	int32_t debugProbe = (probeSettings.debugProbe == 1) ? 1 : 0;

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
	GuiHelper::drawTextAlignedToSize(I18n::tr("Debug probe S/N:"), alignment);
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

	GuiHelper::drawTextAlignedToSize(I18n::tr("SWD speed [kHz]:"), alignment);
	ImGui::SameLine();

	if (ImGui::InputScalar("##speed", ImGuiDataType_U32, &probeSettings.speedkHz, NULL, NULL, "%u"))
		modified = true;

	if (probeSettings.debugProbe == 1)
	{
		drawJlinkTargetName(probeSettings.device, modified);

		if (ImGui::Button("...", ImVec2(35 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
		{
			std::string selectedTargetName = traceProbeDevice->getTargetName();
			if (selectedTargetName.empty())
				acqusitionErrorPopup.show(I18n::tr("Target name"), I18n::tr("No device was picked in the J-Link dialog, the current Target name was kept."), 2.0f);
			else
			{
				probeSettings.device = selectedTargetName;
				modified = true;
			}
		}

		if (probeSettings.device.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight());
			GuiHelper::drawCenteredText(I18n::tr("Target name is empty, the J-Link cannot connect. Enter the device name, e.g. STM32F103C8."));
			ImGui::PopStyleColor();
		}

		GuiHelper::drawTextAlignedToSize(I18n::tr("Keep connection:"), alignment);
		ImGui::SameLine();

		if (ImGui::Checkbox("##keepConnection", &probeSettings.keepConnection))
			modified = true;

		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("Keeps the connection to the probe open when a run ends. Closing it halts the target on some setups, and that halt is what the next run has to undo before it can read anything."));

		GuiHelper::drawTextAlignedToSize(I18n::tr("Connection:"), alignment);
		ImGui::SameLine();

		const char* jlinkConnections[] = {"USB", "TCP/IP"};
		int32_t jlinkConnection = static_cast<int32_t>(probeSettings.connection);

		if (ImGui::Combo("##connection", &jlinkConnection, jlinkConnections, IM_ARRAYSIZE(jlinkConnections)))
		{
			probeSettings.connection = static_cast<ITraceProbe::Connection>(jlinkConnection);
			modified = true;
		}

		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("How the J-Link is reached: over USB, or over the network at the address below."));

		if (probeSettings.connection == ITraceProbe::Connection::Network)
		{
			GuiHelper::drawTextAlignedToSize(I18n::tr("IP address:"), alignment);
			ImGui::SameLine();

			if (ImGui::InputText("##jlinkIp", &probeSettings.ipAddress, 0, NULL, NULL))
				modified = true;

			ImGui::SameLine();
			ImGui::HelpMarker(I18n::tr("A probe reached this way is picked by its address instead of by its serial number."));
		}

		GuiHelper::drawTextAlignedToSize(I18n::tr("JLink script:"), alignment);
		ImGui::SameLine();

		if (ImGui::Checkbox("##scriptEnabled", &probeSettings.scriptEnabled))
			modified = true;

		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("Runs a J-Link script of your own before the target is connected - for targets that need a watchdog stopped or a reset in a particular order."));

		if (probeSettings.scriptEnabled)
		{
			GuiHelper::drawTextAlignedToSize(I18n::tr("script file:"), alignment);
			ImGui::SameLine();

			if (ImGui::InputText("##scriptPath", &probeSettings.scriptPath, 0, NULL, NULL))
				modified = true;

			ImGui::SameLine();

			if (ImGui::Button("...", ImVec2(35 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
			{
				const std::string picked = fileHandler->openFile({{"J-Link script", "JLinkScript"}, {"All files", "*"}});

				if (!picked.empty())
				{
					probeSettings.scriptPath = picked;
					modified = true;
				}
			}
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