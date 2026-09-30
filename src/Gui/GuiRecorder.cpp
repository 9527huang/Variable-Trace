#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "Gui.hpp"
#include "RecorderHandler.hpp"
#include "ViewerDataHandler.hpp"

/* Width the labels are padded to, so the fields line up in one column. */
static constexpr size_t alignment = 20;

/*
 * The recorder window.
 *
 * The recorder does not stream. The target samples into a buffer of its own and
 * the host copies that buffer out afterwards, so the window has two halves that
 * are used one after the other: the upper one configures a run, the lower one
 * shows what came back.
 *
 * Nothing here opens a connection. The recorder works over the probe the
 * acquisition already has open, which is why every action is refused with an
 * explanation while the acquisition is stopped.
 */

bool Gui::syncRecorderSymbols()
{
	/* The two names are the ones the firmware declares, and they are the only
	   handle the host has on the structures. They are looked up again on every
	   detect rather than remembered, because an import that happens later would
	   otherwise leave the previous addresses in place.

	   Absence is the normal state before an *.elf with the recorder sources has
	   been imported, and getVariable throws on a name it does not hold, so the
	   table is asked first. */
	static constexpr const char* settingsName = "____recorderSettings";
	static constexpr const char* recorderName = "____recorder";
	static constexpr const char* driverName = "____serialDriverSettings";

	if (!variableHandler->contains(settingsName) || !variableHandler->contains(recorderName))
	{
		recorderHandler->clearSymbols();
		return false;
	}

	recorderHandler->setSymbols(variableHandler->getVariable(settingsName)->getAddress(),
								variableHandler->getVariable(recorderName)->getAddress());

	/* The serial driver is a separate structure and its absence is not a
	   failure: a firmware built for a hardware probe has no serial driver, and
	   the recorder still works over that probe. */
	recorderHandler->setDriverSymbol(variableHandler->contains(driverName)
										 ? variableHandler->getVariable(driverName)->getAddress()
										 : 0);

	return true;
}

namespace
{
	/* Names of the variables the active recorder group holds, in the order the
	   target will pack them. A variable kept in two plots appears once. */
	std::vector<std::string> recorderSourceNames(const PlotGroup& group)
	{
		std::vector<std::string> names;

		for (auto iterator = group.begin(); iterator != group.end(); ++iterator)
		{
			if (iterator->second.plot == nullptr)
				continue;

			for (const auto& entry : iterator->second.plot->getSeriesMap())
			{
				if (entry.second == nullptr || entry.second->var == nullptr)
					continue;

				const std::string name = entry.second->var->getName();

				if (std::find(names.begin(), names.end(), name) == names.end())
					names.push_back(name);
			}
		}

		return names;
	}

	std::string describePeriod(uint32_t timestepNs, uint32_t skippedSamples)
	{
		const double period = static_cast<double>(timestepNs) * (skippedSamples + 1) * 1e-9;
		char text[96] = {};
		snprintf(text, sizeof(text), "%.4g s per sample (%.4g Hz)", period, period > 0.0 ? 1.0 / period : 0.0);
		return text;
	}
}  // namespace

void Gui::drawRecorderWindow()
{
	if (!showRecorderWindow)
		return;

	if (!ImGui::Begin("Recorder", &showRecorderWindow))
	{
		ImGui::End();
		return;
	}

	/* Kept between frames: the last capture in a form ImPlot can draw, the names
	   that label it, and the two lines the user reads after an action. */
	static std::string statusMessage;
	static std::string errorMessage;
	static std::vector<std::vector<double>> captureValues;
	static std::vector<std::string> captureNames;
	static std::vector<double> captureTimes;

	const auto fail = [&](const std::string& message)
	{
		errorMessage = message;
		statusMessage.clear();
	};

	const auto report = [&](const std::string& message)
	{
		statusMessage = message;
		errorMessage.clear();
	};

	if (recorderHandler == nullptr)
	{
		GuiHelper::drawCenteredText("The recorder is not available in this build.");
		ImGui::End();
		return;
	}

	std::shared_ptr<PlotGroup> group = plotGroupHandler == nullptr ? nullptr : plotGroupHandler->getActiveRecorderGroup();
	std::shared_ptr<IDebugProbe> probe = viewerDataHandler == nullptr ? nullptr : viewerDataHandler->getDebugProbe();
	const bool acquisitionRunning = viewerDataHandler != nullptr && viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN;
	const bool probeOpen = probe != nullptr && acquisitionRunning;

	bool enabled = recorderHandler->isEnabled();

	if (ImGui::Checkbox("Enable recorder", &enabled))
	{
		errorMessage.clear();
		statusMessage.clear();

		/* Turning the subsystem off has to leave the target idle. A timer
		   interrupt that keeps sampling into a buffer nobody will read is
		   exactly the kind of thing that is forgotten about and then blamed for
		   a timing problem elsewhere. */
		if (!enabled && recorderHandler->isRunning() && probe != nullptr)
		{
			std::string error;

			if (recorderHandler->stop(*probe, error))
				report("The recorder was disabled and the target was stopped.");
			else
				fail("The recorder was disabled, but the target could not be stopped: " + error);
		}

		recorderHandler->setEnabled(enabled);
	}

	ImGui::SameLine();
	ImGui::HelpMarker("The recorder samples inside a timer interrupt on the target and keeps the samples in its own memory. Nothing is streamed while it runs.");

	if (group == nullptr)
	{
		ImGui::Separator();
		GuiHelper::drawCenteredText("There is no active recorder group.");
		GuiHelper::drawCenteredText("Add a group of type 'recorder' in the plot tree and make it the active one.");
		ImGui::End();
		return;
	}

	/* The variable list always comes from the group, so a plot added to the tree
	   is part of the next capture without anything having to be kept in step. */
	recorder::Config config = recorderHandler->getPendingConfig();
	std::string groupError;
	const bool groupUsable = recorder::configFromGroup(*group, config, groupError);

	ImGui::Text("Group: %s", group->getName().c_str());

	/* Pushing a change while a run is in progress restarts it, which is what
	   makes the change visible without the user having to stop and start. */
	const auto pushConfig = [&](const recorder::Config& updated, const std::string& what)
	{
		recorderHandler->setPendingConfig(updated);

		if (!recorderHandler->isRunning() || probe == nullptr)
			return;

		std::string error;

		if (recorderHandler->start(*probe, updated, error))
			report("Restarted with the new " + what + ".");
		else
			fail("The recorder could not be restarted: " + error);
	};

	ImGui::Separator();

	/* ------------------------------------------------------------- the target */
	GuiHelper::drawCenteredText("Target");
	ImGui::Separator();

	if (!probeOpen)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight);
		GuiHelper::drawCenteredText("The acquisition is stopped. Start it first, the recorder uses that connection.");
		ImGui::PopStyleColor();
	}

	ImGui::BeginDisabled(!probeOpen);

	if (ImGui::Button("Detect", ImVec2(120 * GuiHelper::contentScale, 0)))
	{
		errorMessage.clear();

		if (!syncRecorderSymbols())
			fail("The symbol table has no ____recorderSettings or ____recorder. Import the variables from an *.elf built with the recorder sources.");
		else
		{
			std::string error;

			if (recorderHandler->detect(*probe, error))
				report("Recorder detected.");
			else
				fail(error);

			/* The driver is read separately and its absence is not an error:
			   the recorder runs over a hardware probe as well, and a firmware
			   built for one carries no serial driver. */
			std::string driverError;

			if (recorderHandler->detectDriver(*probe, driverError))
				report("Recorder detected. " + serial::describeDriverSettings(recorderHandler->getDetectedDriverSettings()) + ".");
		}
	}

	ImGui::SameLine();

	if (recorderHandler->isDetected())
	{
		const recorder::Settings& settings = recorderHandler->getDetectedSettings();
		ImGui::Text("Version %u.%u, time base %u ns, buffer %u elements, up to %u variables, float %s",
					settings.version, settings.revision, settings.timestepNs, settings.maxBufferSize,
					settings.maxVariables, settings.floatSupport != 0 ? "yes" : "no");

		if (recorderHandler->isDriverDetected())
			ImGui::TextDisabled("%s", serial::describeDriverSettings(recorderHandler->getDetectedDriverSettings()).c_str());
	}
	else
		ImGui::TextUnformatted("Not detected yet.");

	ImGui::EndDisabled();

	const bool detected = recorderHandler->isDetected();
	const bool canRun = detected && probeOpen && groupUsable;
	const recorder::Settings& settings = recorderHandler->getDetectedSettings();

	/* ---------------------------------------------------------------- capture */
	GuiHelper::drawCenteredText("Capture");
	ImGui::Separator();

	ImGui::BeginDisabled(!enabled);

	if (!groupUsable)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight);
		ImGui::TextWrapped("%s", groupError.c_str());
		ImGui::PopStyleColor();
	}

	ImGui::BeginDisabled(!canRun);

	const char* modes[] = {"run", "normal", "single"};
	int modeIndex = static_cast<int>(config.mode);

	GuiHelper::drawTextAlignedToSize("Mode:", alignment);
	ImGui::SameLine();

	if (ImGui::Combo("##recorderMode", &modeIndex, modes, IM_ARRAYSIZE(modes)))
	{
		errorMessage.clear();

		recorder::Mode mode = static_cast<recorder::Mode>(modeIndex);

		/* Nothing arms the comparison in run mode, so a mode that waits for a
		   trigger without one would look like a trigger that never fired. */
		if (mode != recorder::Mode::Run && (!config.trigger.enabled || config.trigger.source.empty()))
			fail("Mode '" + std::string(modes[modeIndex]) + "' waits for a trigger. Pick a source variable below first.");
		else
		{
			recorder::Config updated = config;
			updated.mode = mode;
			pushConfig(updated, "mode");
		}
	}

	ImGui::SameLine();
	ImGui::HelpMarker("Run captures continuously and keeps the newest samples. Normal and single wait for the trigger; "
					  "single stops after one capture, normal can be started again.");

	GuiHelper::drawTextAlignedToSize("Skip samples:", alignment);
	ImGui::SameLine();

	int skipped = static_cast<int>(config.skippedSamples);

	if (ImGui::InputInt("##skippedSamples", &skipped, 1, 10))
	{
		skipped = std::max(0, skipped);
		recorder::Config updated = config;
		updated.skippedSamples = static_cast<uint32_t>(skipped);
		pushConfig(updated, "downsampling");
	}

	ImGui::SameLine();

	if (detected)
		ImGui::TextDisabled("%s", describePeriod(settings.timestepNs, config.skippedSamples).c_str());
	else
		ImGui::TextDisabled("Time base unknown until the recorder is detected.");

	const uint32_t packSize = recorder::packSizeOf(config.variables);

	if (groupUsable)
	{
		GuiHelper::drawTextAlignedToSize("Sample:", alignment);
		ImGui::SameLine();
		ImGui::Text("%u variables, %u bytes", static_cast<uint32_t>(config.variables.size()), packSize);
	}

	/* ---------------------------------------------------------------- trigger */
	ImGui::BeginDisabled(config.mode == recorder::Mode::Run);

	GuiHelper::drawCenteredText("Trigger");
	ImGui::Separator();

	const std::vector<std::string> sourceNames = recorderSourceNames(*group);
	int sourceIndex = -1;
	std::string sourcePreview = config.trigger.source.empty() ? "none" : config.trigger.source;

	for (size_t index = 0; index < sourceNames.size(); index++)
	{
		if (sourceNames[index] == config.trigger.source)
			sourceIndex = static_cast<int>(index);
	}

	GuiHelper::drawTextAlignedToSize("Source:", alignment);
	ImGui::SameLine();

	if (ImGui::BeginCombo("##triggerSource", sourcePreview.c_str()))
	{
		for (size_t index = 0; index < sourceNames.size(); index++)
		{
			const bool selected = static_cast<int>(index) == sourceIndex;

			if (ImGui::Selectable(sourceNames[index].c_str(), selected))
			{
				recorder::Config updated = config;
				updated.trigger.source = sourceNames[index];
				updated.trigger.enabled = true;
				pushConfig(updated, "trigger source");
			}

			if (selected)
				ImGui::SetItemDefaultFocus();
		}

		ImGui::EndCombo();
	}

	GuiHelper::drawTextAlignedToSize("Edge:", alignment);
	ImGui::SameLine();

	const char* edges[] = {"rising", "falling"};
	int edgeIndex = static_cast<int>(config.trigger.edge);

	if (ImGui::Combo("##triggerEdge", &edgeIndex, edges, IM_ARRAYSIZE(edges)))
	{
		recorder::Config updated = config;
		updated.trigger.edge = static_cast<recorder::TriggerEdge>(edgeIndex);
		pushConfig(updated, "trigger edge");
	}

	GuiHelper::drawTextAlignedToSize("Value:", alignment);
	ImGui::SameLine();

	double triggerValue = config.trigger.value;

	if (ImGui::InputDouble("##triggerValue", &triggerValue, 0.0, 0.0, "%.6g"))
	{
		recorder::Config updated = config;
		updated.trigger.value = triggerValue;
		recorderHandler->setPendingConfig(updated);
	}

	GuiHelper::drawTextAlignedToSize("Pre-trigger:", alignment);
	ImGui::SameLine();

	int preTrigger = static_cast<int>(config.trigger.preTriggerSamples);

	if (ImGui::InputInt("##preTrigger", &preTrigger, 1, 10))
	{
		preTrigger = std::max(0, preTrigger);
		recorder::Config updated = config;
		updated.trigger.preTriggerSamples = static_cast<uint32_t>(preTrigger);
		recorderHandler->setPendingConfig(updated);
	}

	ImGui::SameLine();
	ImGui::HelpMarker("Samples kept from before the trigger. The captured window always ends at the trigger plus this many samples.");

	ImGui::EndDisabled();

	/* ------------------------------------------------------------------- run */
	ImGui::Separator();

	const bool running = recorderHandler->isRunning();

	if (running)
	{
		if (ImGui::Button("Stop", ImVec2(120 * GuiHelper::contentScale, 0)))
		{
			std::string error;

			if (recorderHandler->stop(*probe, error))
				report("The target was stopped.");
			else
				fail(error);
		}
	}
	else if (ImGui::Button("Start", ImVec2(120 * GuiHelper::contentScale, 0)))
	{
		errorMessage.clear();

		if (!syncRecorderSymbols())
			fail("The symbol table has no ____recorderSettings or ____recorder. Import the variables from an *.elf built with the recorder sources.");
		else
		{
			std::string error;

			if (recorderHandler->detect(*probe, error) && recorderHandler->start(*probe, config, error))
				report("Recording. The target fills its buffer on its own, press Download when it has filled.");
			else
				fail(error);
		}
	}

	ImGui::SameLine();
	ImGui::TextUnformatted(running ? "The target is sampling." : "The target is idle.");

	ImGui::EndDisabled();  // canRun
	ImGui::EndDisabled();  // enabled

	/* -------------------------------------------------------------- download */
	ImGui::Separator();

	ImGui::BeginDisabled(!probeOpen || !recorderHandler->isDetected());

	if (ImGui::Button("Download", ImVec2(120 * GuiHelper::contentScale, 0)))
	{
		errorMessage.clear();

		std::string error;
		recorder::Capture downloaded;

		/* The copy walks the buffer over the connection the sampling loop reads.
		   It is taken under the same lock rather than by stopping the loop, which
		   would close the very connection the copy needs. */
		std::unique_lock<std::mutex> probeLock = viewerDataHandler->lockProbe();

		if (recorderHandler->download(*probe, config.variables, downloaded, error))
		{
			captureNames.clear();

			for (const recorder::Slot& slot : config.variables)
				captureNames.push_back(slot.name);

			captureTimes = downloaded.timestamps;
			captureValues.assign(downloaded.series.size(), {});

			for (size_t index = 0; index < downloaded.series.size(); index++)
			{
				captureValues[index].reserve(downloaded.series[index].size());

				for (uint32_t raw : downloaded.series[index])
					captureValues[index].push_back(static_cast<double>(raw));
			}

			report("Copied " + std::to_string(recorderHandler->getLastDownloadSize()) + " bytes: " +
				   std::to_string(downloaded.samples) + " samples.");
		}
		else
			fail(error);
	}

	ImGui::SameLine();
	ImGui::HelpMarker("Copies the whole buffer out of the target and rebuilds the samples from the ring state. "
					  "The sampling loop pauses for the duration of the copy.");

	ImGui::EndDisabled();

	if (!errorMessage.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight);
		ImGui::TextWrapped("%s", errorMessage.c_str());
		ImGui::PopStyleColor();
	}

	if (!statusMessage.empty())
		ImGui::TextWrapped("%s", statusMessage.c_str());

	if (!captureValues.empty() && !captureTimes.empty())
	{
		/* The full buffer is drawn: what a target recorder holds is small enough
		   that decimating it would hide the very features it is used to find. */
		if (ImPlot::BeginPlot("##recorderCapture", ImVec2(-1, 300 * GuiHelper::contentScale)))
		{
			ImPlot::SetupAxes("time [s]", "value");

			for (size_t index = 0; index < captureValues.size(); index++)
			{
				const std::string label = index < captureNames.size() ? captureNames[index] : ("variable " + std::to_string(index));

				ImPlot::PlotLine(label.c_str(), captureTimes.data(), captureValues[index].data(),
								 static_cast<int>(captureValues[index].size()));
			}

			ImPlot::EndPlot();
		}

		ImGui::Text("%u samples, one every %s", static_cast<uint32_t>(captureTimes.size()),
					describePeriod(settings.timestepNs, config.skippedSamples).c_str());
	}

	ImGui::End();
}
