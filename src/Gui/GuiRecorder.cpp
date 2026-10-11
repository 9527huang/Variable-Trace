#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Gui.hpp"
#include "I18n.hpp"
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
	   target will pack them. A variable kept in two plots appears once.

	   A variable that is worked out from another one - a scaled value, a pair of
	   error bits - is left out. The recorder compares the trigger in the target,
	   against the raw bytes of a variable on the target, and a derived value has
	   no bytes of its own to compare. Offering it would arm a comparison against
	   whatever address happened to be attached to it, and the trigger would then
	   look like one that never fires.

	   The second return says whether anything was left out, so the window can
	   explain an empty list instead of showing a bare one. */
	std::vector<std::string> recorderSourceNames(const PlotGroup& group, bool& skippedDerived)
	{
		std::vector<std::string> names;
		skippedDerived = false;

		for (auto iterator = group.begin(); iterator != group.end(); ++iterator)
		{
			if (iterator->second.plot == nullptr)
				continue;

			for (const auto& entry : iterator->second.plot->getSeriesMap())
			{
				if (entry.second == nullptr || entry.second->var == nullptr)
					continue;

				if (entry.second->var->getFractional().baseVariable != nullptr)
				{
					skippedDerived = true;
					continue;
				}

				const std::string name = entry.second->var->getName();

				if (std::find(names.begin(), names.end(), name) == names.end())
					names.push_back(name);
			}
		}

		return names;
	}

	/* Time one sample takes, in seconds. The drag line for the pre-trigger runs
	   on the time axis, so the sample count and the seconds it stands for have to
	   come out of one place - otherwise a change to the period would move the
	   line and the number apart. */
	double describePeriodSeconds(uint32_t timestepNs, uint32_t skippedSamples)
	{
		return static_cast<double>(timestepNs) * (static_cast<double>(skippedSamples) + 1.0) * 1e-9;
	}

	std::string describePeriod(uint32_t timestepNs, uint32_t skippedSamples)
	{
		const double period = describePeriodSeconds(timestepNs, skippedSamples);
		char text[96] = {};
		snprintf(text, sizeof(text), I18n::tr("%.4g s per sample (%.4g Hz)"), period, period > 0.0 ? 1.0 / period : 0.0);
		return text;
	}
}  // namespace

void Gui::drawRecorderWindow()
{
	if (!showRecorderWindow)
		return;

	if (!ImGui::Begin(I18n::windowTitle("Recorder").c_str(), &showRecorderWindow))
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
	/* Whether the samples that were overwritten had to be given up to get this
	   copy. Read with the capture because it describes that capture, not the
	   target as it stands now. */
	static bool captureOverran = false;
	static bool captureStartMoved = false;

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
		GuiHelper::drawCenteredText(I18n::tr("The recorder is not available in this build."));
		ImGui::End();
		return;
	}

	std::shared_ptr<PlotGroup> group = plotGroupHandler == nullptr ? nullptr : plotGroupHandler->getActiveRecorderGroup();
	std::shared_ptr<IDebugProbe> probe = viewerDataHandler == nullptr ? nullptr : viewerDataHandler->getDebugProbe();
	const bool acquisitionRunning = viewerDataHandler != nullptr && viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN;
	const bool probeOpen = probe != nullptr && acquisitionRunning;

	bool enabled = recorderHandler->isEnabled();

	if (ImGui::Checkbox(I18n::tr("Enable recorder"), &enabled))
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
				report(I18n::tr("The recorder was disabled and the target was stopped."));
			else
				fail(I18n::tr("The recorder was disabled, but the target could not be stopped: ") + error);
		}

		recorderHandler->setEnabled(enabled);
	}

	ImGui::SameLine();
	ImGui::HelpMarker(I18n::tr("The recorder samples inside a timer interrupt on the target and keeps the samples in its own memory. Nothing is streamed while it runs."));

	if (group == nullptr)
	{
		ImGui::Separator();
		GuiHelper::drawCenteredText(I18n::tr("There is no active recorder group."));
		GuiHelper::drawCenteredText(I18n::tr("Add a group of type 'recorder' in the plot tree and make it the active one."));
		ImGui::End();
		return;
	}

	/* The variable list always comes from the group, so a plot added to the tree
	   is part of the next capture without anything having to be kept in step. */
	recorder::Config config = recorderHandler->getPendingConfig();
	std::string groupError;
	const bool groupUsable = recorder::configFromGroup(*group, config, groupError);

	ImGui::Text(I18n::tr("Group: %s"), group->getName().c_str());

	/* Pushing a change while a run is in progress restarts it, which is what
	   makes the change visible without the user having to stop and start. */
	const auto pushConfig = [&](const recorder::Config& updated, const std::string& what)
	{
		recorderHandler->setPendingConfig(updated);

		if (!recorderHandler->isRunning() || probe == nullptr)
			return;

		std::string error;

		if (recorderHandler->start(*probe, updated, error))
		{
			/* The setting that changed is named in the middle of the sentence,
			   and a language is free to put it somewhere else, so the wording
			   goes through the table as a whole. The name itself is a key the
			   configuration and the API also use, and stays as it is. */
			char message[256] = {};
			snprintf(message, sizeof(message), I18n::tr("Restarted with the new %s."), what.c_str());
			report(message);
		}
		else
			fail(I18n::tr("The recorder could not be restarted: ") + error);
	};

	ImGui::Separator();

	/* ------------------------------------------------------------- the target */
	GuiHelper::drawCenteredText(I18n::tr("Target"));
	ImGui::Separator();

	if (!probeOpen)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight());
		GuiHelper::drawCenteredText(I18n::tr("The acquisition is stopped. Start it first, the recorder uses that connection."));
		ImGui::PopStyleColor();
	}

	ImGui::BeginDisabled(!probeOpen);

	if (ImGui::Button(I18n::tr("Detect"), ImVec2(120 * GuiHelper::contentScale, 0)))
	{
		errorMessage.clear();

		if (!syncRecorderSymbols())
			fail(I18n::tr("The symbol table has no ____recorderSettings or ____recorder. Import the variables from an *.elf/*.axf built with the recorder sources."));
		else
		{
			std::string error;

			if (recorderHandler->detect(*probe, error))
				report(I18n::tr("Recorder detected."));
			else
				fail(error);

			/* The driver is read separately and its absence is not an error:
			   the recorder runs over a hardware probe as well, and a firmware
			   built for one carries no serial driver. */
			std::string driverError;

			if (recorderHandler->detectDriver(*probe, driverError))
			{
				char message[512] = {};
				snprintf(message, sizeof(message), I18n::tr("Recorder detected. %s."), serial::describeDriverSettings(recorderHandler->getDetectedDriverSettings()).c_str());
				report(message);
			}
		}
	}

	ImGui::SameLine();

	if (recorderHandler->isDetected())
	{
		const recorder::Settings& settings = recorderHandler->getDetectedSettings();
		ImGui::Text(I18n::tr("Version %u.%u, time base %u ns, buffer %u elements, up to %u variables, float %s"),
					settings.version, settings.revision, settings.timestepNs, settings.maxBufferSize,
					settings.maxVariables, settings.floatSupport != 0 ? "yes" : "no");

		if (recorderHandler->isDriverDetected())
			ImGui::TextDisabled("%s", serial::describeDriverSettings(recorderHandler->getDetectedDriverSettings()).c_str());
	}
	else
		ImGui::TextUnformatted(I18n::tr("Not detected yet."));

	ImGui::EndDisabled();

	const bool detected = recorderHandler->isDetected();
	const bool canRun = detected && probeOpen && groupUsable;
	const recorder::Settings& settings = recorderHandler->getDetectedSettings();

	/* ---------------------------------------------------------------- capture */
	GuiHelper::drawCenteredText(I18n::tr("Capture"));
	ImGui::Separator();

	ImGui::BeginDisabled(!enabled);

	if (!groupUsable)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight());
		ImGui::TextWrapped("%s", groupError.c_str());
		ImGui::PopStyleColor();
	}

	ImGui::BeginDisabled(!canRun);

	const char* modes[] = {I18n::tr("run"), I18n::tr("normal"), I18n::tr("single")};
	int modeIndex = static_cast<int>(config.mode);

	GuiHelper::drawTextAlignedToSize(I18n::tr("Mode:"), alignment);
	ImGui::SameLine();

	if (ImGui::Combo("##recorderMode", &modeIndex, modes, IM_ARRAYSIZE(modes)))
	{
		errorMessage.clear();

		recorder::Mode mode = static_cast<recorder::Mode>(modeIndex);

		/* Nothing arms the comparison in run mode, so a mode that waits for a
		   trigger without one would look like a trigger that never fired. */
		if (mode != recorder::Mode::Run && (!config.trigger.enabled || config.trigger.source.empty()))
			fail(std::string(I18n::tr("Mode '")) + modes[modeIndex] + I18n::tr("' waits for a trigger. Pick a source variable below first."));
		else
		{
			recorder::Config updated = config;
			updated.mode = mode;
			pushConfig(updated, "mode");
		}
	}

	ImGui::SameLine();
	ImGui::HelpMarker(I18n::tr("Run captures continuously and keeps the newest samples. Normal and single wait for the trigger; "
							   "single stops after one capture, normal can be started again."));

	GuiHelper::drawTextAlignedToSize(I18n::tr("Skip samples:"), alignment);
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
		ImGui::TextDisabled(I18n::tr("Time base unknown until the recorder is detected."));

	const uint32_t packSize = recorder::packSizeOf(config.variables);

	if (groupUsable)
	{
		GuiHelper::drawTextAlignedToSize(I18n::tr("Sample:"), alignment);
		ImGui::SameLine();
		ImGui::Text(I18n::tr("%u variables, %u bytes"), static_cast<uint32_t>(config.variables.size()), packSize);
	}

	/* ---------------------------------------------------------------- trigger */
	ImGui::BeginDisabled(config.mode == recorder::Mode::Run);

	GuiHelper::drawCenteredText(I18n::tr("Trigger"));
	ImGui::Separator();

	bool skippedDerived = false;
	const std::vector<std::string> sourceNames = recorderSourceNames(*group, skippedDerived);
	int sourceIndex = -1;
	std::string sourcePreview = config.trigger.source.empty() ? "none" : config.trigger.source;

	for (size_t index = 0; index < sourceNames.size(); index++)
	{
		if (sourceNames[index] == config.trigger.source)
			sourceIndex = static_cast<int>(index);
	}

	GuiHelper::drawTextAlignedToSize(I18n::tr("Source:"), alignment);
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

		if (sourceNames.empty())
			ImGui::TextDisabled("%s", I18n::tr("No variable of this group can be a trigger source."));

		ImGui::EndCombo();
	}

	/* An empty list is not the same as a group with nothing in it, so the reason
	   is named rather than left for the user to work out. */
	if (sourceNames.empty() && skippedDerived)
	{
		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("A variable worked out from another one has no bytes of its own on the target, "
								  "so the recorder cannot compare it. Add a plain variable to the group to trigger on it."));
	}

	GuiHelper::drawTextAlignedToSize(I18n::tr("Edge:"), alignment);
	ImGui::SameLine();

	const char* edges[] = {I18n::tr("rising"), I18n::tr("falling")};
	int edgeIndex = static_cast<int>(config.trigger.edge);

	if (ImGui::Combo("##triggerEdge", &edgeIndex, edges, IM_ARRAYSIZE(edges)))
	{
		recorder::Config updated = config;
		updated.trigger.edge = static_cast<recorder::TriggerEdge>(edgeIndex);
		pushConfig(updated, "trigger edge");
	}

	GuiHelper::drawTextAlignedToSize(I18n::tr("Value:"), alignment);
	ImGui::SameLine();

	double triggerValue = config.trigger.value;

	if (ImGui::InputDouble("##triggerValue", &triggerValue, 0.0, 0.0, "%.6g"))
	{
		recorder::Config updated = config;
		updated.trigger.value = triggerValue;
		recorderHandler->setPendingConfig(updated);
	}

	GuiHelper::drawTextAlignedToSize(I18n::tr("Pre-trigger:"), alignment);
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
	ImGui::HelpMarker(I18n::tr("Samples kept from before the trigger. The captured window always ends at the trigger plus this many samples."));

	ImGui::EndDisabled();

	/* ------------------------------------------------------------------- run */
	ImGui::Separator();

	/* The trigger state is asked of the target at most a few times a second: it
	   costs a round trip on the link the sampling loop shares, and the answer only
	   changes when the signal crosses the level, so asking every frame would spend
	   the link on a number that is nearly always the same. */
	static double lastTriggerPoll = 0.0;
	static recorder::TriggerState lastTriggerState = recorder::TriggerState::Setup;

	const bool triggeredMode = config.mode != recorder::Mode::Run;
	const bool running = recorderHandler->isRunning();

	if (running)
	{
		if (ImGui::Button(I18n::tr("Stop"), ImVec2(120 * GuiHelper::contentScale, 0)))
		{
			std::string error;

			if (recorderHandler->stop(*probe, error))
				report(I18n::tr("The target was stopped."));
			else
				fail(error);
		}
	}
	else if (ImGui::Button(I18n::tr("Start"), ImVec2(120 * GuiHelper::contentScale, 0)))
	{
		errorMessage.clear();

		if (!syncRecorderSymbols())
			fail(I18n::tr("The symbol table has no ____recorderSettings or ____recorder. Import the variables from an *.elf/*.axf built with the recorder sources."));
		else
		{
			std::string error;

			if (recorderHandler->detect(*probe, error) && recorderHandler->start(*probe, config, error))
			{
				/* A fresh run has not fired yet, whatever the last one did. */
				lastTriggerState = recorder::TriggerState::Setup;
				lastTriggerPoll = ImGui::GetTime();
				report(I18n::tr("Recording. The target fills its buffer on its own, press Download when it has filled."));
			}
			else
				fail(error);
		}
	}

	ImGui::SameLine();

	/* The state of the target, said in terms the user is working in. A run in one
	   of the triggered modes spends most of its time waiting, and "sampling" does
	   not tell them apart from a run that has already caught something. */
	if (running && triggeredMode && probe != nullptr)
	{
		const double now = ImGui::GetTime();

		if (now - lastTriggerPoll >= 0.25)
		{
			lastTriggerPoll = now;

			std::string pollError;

			if (!recorderHandler->readTriggerState(*probe, lastTriggerState, pollError))
				lastTriggerState = recorder::TriggerState::Setup;
		}
	}

	if (!running)
		ImGui::TextUnformatted(I18n::tr("The target is idle."));
	else if (!triggeredMode)
		ImGui::TextUnformatted(I18n::tr("The target is sampling."));
	else if (lastTriggerState == recorder::TriggerState::Triggered || lastTriggerState == recorder::TriggerState::Posttrigger)
		ImGui::TextUnformatted(I18n::tr("The trigger has fired."));
	else
		ImGui::TextUnformatted(I18n::tr("Waiting for trigger..."));

	ImGui::EndDisabled();  // canRun
	ImGui::EndDisabled();  // enabled

	/* -------------------------------------------------------------- download */
	ImGui::Separator();

	ImGui::BeginDisabled(!probeOpen || !recorderHandler->isDetected());

	if (ImGui::Button(I18n::tr("Download"), ImVec2(120 * GuiHelper::contentScale, 0)))
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
			captureOverran = downloaded.overran;

			/* The start the target handed over did not sit on a sample boundary,
			   so the window was moved down by less than one sample before the
			   bytes were split. Said out loud because the values are still good:
			   this is a note about where the capture begins, not a fault. */
			captureStartMoved =
				downloaded.ring.packSize != 0 && (downloaded.ring.start % downloaded.ring.packSize) != 0;
			captureValues.assign(downloaded.series.size(), {});

			for (size_t index = 0; index < downloaded.series.size(); index++)
			{
				/* What came back is raw bytes, and a float channel is its bit
				   pattern until its type is applied - which is done here, once,
				   so the preview and the export both read the values the variable
				   itself would read. */
				const bool known = variableHandler != nullptr && index < config.variables.size() &&
								   variableHandler->contains(config.variables[index].name);
				std::shared_ptr<Variable> variable = known ? variableHandler->getVariable(config.variables[index].name) : nullptr;

				captureValues[index].reserve(downloaded.series[index].size());

				for (uint32_t raw : downloaded.series[index])
					captureValues[index].push_back(variable != nullptr ? variable->transformToDouble(raw)
																		: static_cast<double>(raw));
			}

			report("Copied " + std::to_string(recorderHandler->getLastDownloadSize()) + " bytes: " +
				   std::to_string(downloaded.samples) + " samples.");
		}
		else
			fail(error);
	}

	ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("Copies the whole buffer out of the target and rebuilds the samples from the ring state. "
								   "The sampling loop pauses for the duration of the copy."));

	ImGui::EndDisabled();

	/* The capture is written beside the project, named after it and the moment:
	   a second capture does not overwrite the first, and nothing stands between
	   the button and the file. The columns carry what the variables' own types
	   make of the bytes - what came back is raw, and a float channel is its bit
	   pattern until its type is applied. */
	if (ImGui::Button(I18n::tr("Export capture to *.csv"), ImVec2(200 * GuiHelper::contentScale, 0)))
	{
		errorMessage.clear();

		if (captureValues.empty() || captureTimes.empty())
			fail(I18n::tr("There is nothing to write yet. Press Download first."));
		else
		{
			std::filesystem::path target(projectConfigPath);
			target.replace_extension();

			const std::time_t now = std::time(nullptr);
			std::tm local = {};
			localtime_s(&local, &now);

			char stamp[32] = {};
			std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

			const std::string path = target.string() + "_recorder_" + stamp + ".csv";
			std::ofstream file(path);

			if (!file)
				fail(I18n::tr("The capture could not be written to ") + path);
			else
			{
				file << "time [s]";

				for (const std::string& name : captureNames)
					file << "," << name;

				file << "\n";

				const size_t samples = captureTimes.size();

				for (size_t index = 0; index < samples; ++index)
				{
					file << captureTimes[index];

					for (size_t column = 0; column < captureValues.size() && column < captureNames.size(); ++column)
					{
						if (index < captureValues[column].size())
							file << "," << captureValues[column][index];
						else
							file << ",";
					}

					file << "\n";
				}

				report(I18n::tr("Wrote ") + path + " (" + std::to_string(samples) + " samples).");
			}
		}
	}

	ImGui::SameLine();
	ImGui::HelpMarker(I18n::tr("Writes the capture beside the project file, one column per variable, "
							   "with the values the variables' own types give them."));

	if (!errorMessage.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::redLight());
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
			ImPlot::SetupAxes(I18n::tr("time [s]"), I18n::tr("value"));

			/* The trigger is set from here as well as from the number field, the
			   same way the trace view does it: the level is a line the user drags
			   until it sits where the signal crosses it, and the number follows.

			   Both lines only mean something in the two triggered modes, so they
			   are drawn only there. In run mode nothing is compared, and a line
			   that can be dragged but changes nothing is a control that lies. */
			if (config.mode != recorder::Mode::Run)
			{
				auto triggerLevel = config.trigger.value;

				if (ImPlot::DragLineY(0, &triggerLevel, GuiHelper::triggerLine(), 1.0f))
				{
					recorder::Config updated = config;
					updated.trigger.value = triggerLevel;
					pushConfig(updated, "trigger level");
				}

				/* The capture starts at the oldest sample and the trigger sits
				   where the pre-trigger run ends, so the boundary is that many
				   samples in. A one-sample minimum keeps the line visible when
				   nothing was kept from before the trigger. */
				const double period = describePeriodSeconds(settings.timestepNs, config.skippedSamples);
				double boundary = static_cast<double>(config.trigger.preTriggerSamples) * period;

				if (boundary <= 0.0)
					boundary = period;

				if (ImPlot::DragLineX(1, &boundary, GuiHelper::triggerLine(), 1.0f))
				{
					/* Converted back through the same period, rounded rather than
					   truncated so that dragging to the left edge lands on zero
					   instead of on a sample that is always one short. */
					const long long samples = std::llround(boundary / period);

					recorder::Config updated = config;
					updated.trigger.preTriggerSamples = static_cast<uint32_t>(std::max<long long>(0, samples));
					pushConfig(updated, "pre-trigger");
				}
			}

			for (size_t index = 0; index < captureValues.size(); index++)
			{
				const std::string label = index < captureNames.size() ? captureNames[index] : ("variable " + std::to_string(index));

				ImPlot::PlotLine(label.c_str(), captureTimes.data(), captureValues[index].data(),
								 static_cast<int>(captureValues[index].size()));
			}

			ImPlot::EndPlot();
		}

		ImGui::Text(I18n::tr("%u samples, one every %s"), static_cast<uint32_t>(captureTimes.size()),
					describePeriod(settings.timestepNs, config.skippedSamples).c_str());

		/* An overrun is said out loud because the capture looks complete either
		   way: the samples that are there are correct, the ones that were
		   overwritten are simply absent, and its axis still starts at zero. */
		ImGui::SameLine();

		if (captureOverran)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::orangeLight());
			ImGui::TextUnformatted(I18n::tr("Older samples were overwritten before the copy."));
			ImGui::PopStyleColor();
		}
		else
			ImGui::TextDisabled("%s", I18n::tr("No samples were lost."));

		/* The start the target reported was not where a sample begins, so the
		   copy was taken from the boundary just below it. The values read back
		   are correct; what changes is that the capture may begin up to one
		   sample later than the target meant it to. */
		if (captureStartMoved)
		{
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, GuiHelper::orangeLight());
			ImGui::TextUnformatted(I18n::tr("The capture start did not sit on a sample boundary, so it was moved down to the nearest one."));
			ImGui::PopStyleColor();
		}
	}

	ImGui::End();
}
