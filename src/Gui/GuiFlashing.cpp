#include <imgui.h>

#include <cstdio>
#include <string>
#include <vector>

#include "FlashingService.hpp"
#include "Gui.hpp"
#include "ViewerDataHandler.hpp"

/*
 * Flashing: the panel that shows what the programmer prints, and the settings
 * that say which programmer to run.
 *
 * A flash erases and rewrites the target, so the acquisition cannot be left
 * running while it happens. Starting a flash stops the acquisition first and
 * says so in the log, otherwise the curves would simply stop and the reason
 * would be invisible.
 *
 * Only one run is in flight at a time. The service refuses a second one, so
 * both buttons ask it before they start anything.
 */

/* Width the labels are padded to, so the fields line up in one column, as in
   the acquisition settings window. */
/* Wide enough for the longest label in this window, which is the one about
   taking the symbol file of the acquisition settings. */
static constexpr size_t alignment = 22;

/* Cycled through by the button label while a run is in progress. A programmer
   that takes a minute to say anything would otherwise look like a button that
   did nothing. */
static constexpr char spinnerCharacters[] = "|/-\\";

std::string Gui::resolveFirmwareFile() const
{
	const FlashingService::FlashSettings& settings = globalConfig->getSettings().flash;

	/* The *.elf of the acquisition settings is what a build leaves behind, so
	   the setting replaces the picked path rather than sitting next to it. It is
	   stored relative to the project file, and the programmer runs with the
	   working directory of the application, so it is turned absolute here. */
	if (settings.useElfFile)
		return GuiHelper::convertProjectPathToAbsolute(&projectElfPath, &projectConfigPath);

	return settings.file;
}

const std::vector<std::string>& Gui::flashOutput()
{
	/* The panels redraw about sixty times a second and a programmer can print
	   thousands of lines, so the buffer is copied only when the service says a
	   line has arrived. The count it returns resets when a run starts, which
	   makes it differ from the cached one and refreshes the copy. */
	const size_t revision = flasher->getOutputRevision();

	if (revision != flashOutputRevision)
	{
		flashOutputRevision = revision;
		flashOutputLines = flasher->getOutput();
	}

	return flashOutputLines;
}

void Gui::startFlash(bool test)
{
	const FlashingService::FlashSettings& settings = globalConfig->getSettings().flash;

	/* Asked before anything else, because a command that is not there is the
	   one thing the user can fix in a single step. */
	if (settings.command.empty())
	{
		flashPopup.show("Error!", test ? "Set a flash command above first." : "Set a flash command in Options -> Flashing.", 2.5f);
		return;
	}

	if (flasher->isRunning())
		return;

	const std::string file = resolveFirmwareFile();

	if (file.empty())
	{
		flashPopup.show("Error!", "No firmware file configured. Provide 'file' or set it in Options -> Flashing.", 2.5f);
		return;
	}

	/* Nothing may read the target while it is being erased and rewritten. */
	if (viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN)
	{
		viewerDataHandler->setState(DataHandlerBase::State::STOP);
		logger->info("Acquisition stopped for flashing");
	}

	lastFlashKind = test ? FlashKind::Test : FlashKind::Real;

	if (!test)
		showFlashOutputWindow = true;

	/* A timeout of zero means "never give up", which is what an unticked box
	   has to turn into for the service. */
	const int32_t timeout = settings.timeoutEnabled ? settings.timeoutSeconds : 0;

	if (!flasher->startFlash(settings.command, file, timeout).valid())
	{
		flashPopup.show("Error!", "A flash is already in progress.", 2.0f);
		return;
	}

	logger->info("Flash started, file: {}", file);
}

void Gui::drawFlashButton()
{
	const FlashingService::State state = flasher->getState();
	const bool running = (state == FlashingService::State::Running);

	/* The label carries the spinner, which is the only sign of life available
	   while a programmer is quiet. */
	char label[16] = {};

	if (running)
	{
		const size_t phase = static_cast<size_t>(ImGui::GetTime() * 8.0) % (sizeof(spinnerCharacters) - 1);
		snprintf(label, sizeof(label), "Flash %c", spinnerCharacters[phase]);
	}
	else
		snprintf(label, sizeof(label), "Flash");

	if (running)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, GuiHelper::green);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GuiHelper::greenLight);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, GuiHelper::greenLightDim);
	}

	if (ImGui::Button(label, ImVec2(-1, 25 * GuiHelper::contentScale)))
		startFlash(false);

	if (running)
		ImGui::PopStyleColor(3);

	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", running ? "A flash is in progress. See the Flash Output window."
										: "Runs the flash command from Options -> Flashing on the firmware file.");
}

void Gui::drawFlashOutputWindow()
{
	if (!showFlashOutputWindow)
		return;

	if (!ImGui::Begin("Flash Output", &showFlashOutputWindow))
	{
		ImGui::End();
		return;
	}

	const FlashingService::State state = flasher->getState();
	const std::vector<std::string>& lines = flashOutput();
	const size_t dropped = flasher->getDroppedLineCount();

	if (state == FlashingService::State::Running)
	{
		ImGui::TextColored(GuiHelper::orangeLight, "Flashing in progress...");
		ImGui::SameLine();

		if (ImGui::Button("Abort", ImVec2(80 * GuiHelper::contentScale, 0)))
			flasher->abort();
	}
	else if (state == FlashingService::State::Success)
	{
		ImGui::TextColored(GuiHelper::greenLight, "Flash successful!");
	}
	else if (state != FlashingService::State::Idle)
	{
		/* The service words the failure: aborted by the user, cut short because
		   it stopped printing, or an exit code. */
		ImGui::TextColored(GuiHelper::redLight, "%s", flasher->getLastError().c_str());
	}

	ImGui::TextUnformatted("Flash command output:");
	ImGui::SameLine();

	if (dropped > 0)
		ImGui::TextDisabled("(%zu earlier lines were dropped)", dropped);
	else
		ImGui::TextDisabled("(the last %zu lines are kept)", FlashingService::maximumOutputLines);

	ImGui::BeginChild("flashOut", ImVec2(-1, -1), true);

	/* Follows the tail only while the view is already at the bottom, so that
	   scrolling back to read something is not undone by the next line. */
	const bool wasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

	if (lines.empty())
	{
		ImGui::TextDisabled("(no output)");
	}
	else
	{
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(lines.size()));

		while (clipper.Step())
		{
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
				ImGui::TextUnformatted(lines[static_cast<size_t>(row)].c_str());
		}
	}

	if (state == FlashingService::State::Running && wasAtBottom)
		ImGui::SetScrollHereY(1.0f);

	ImGui::EndChild();
	ImGui::End();
}

void Gui::drawFlashingSettingsWindow()
{
	if (showFlashingSettingsWindow)
		ImGui::OpenPopup("Flashing Settings");

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(720 * GuiHelper::contentScale, 600 * GuiHelper::contentScale));

	if (!ImGui::BeginPopupModal("Flashing Settings", &showFlashingSettingsWindow, 0))
		return;

	/* Every field edits the stored setting directly, one field at a time. A
	   copy that was written back at the end of the frame would silently undo a
	   change an API client made while the window was open. */
	FlashingService::FlashSettings& settings = globalConfig->getSettings().flash;

	ImGui::Dummy(ImVec2(-1, 5));
	GuiHelper::drawCenteredText("Select firmware file and flash command to use for flashing.");
	ImGui::Separator();

	/* FIRMWARE FILE */
	GuiHelper::drawTextAlignedToSize("Firmware file:", alignment);
	ImGui::SameLine();

	ImGui::BeginDisabled(settings.useElfFile);
	ImGui::InputText("##flashingFile", &settings.file, 0, NULL, NULL);
	ImGui::SameLine();

	if (ImGui::Button("...", ImVec2(35 * GuiHelper::contentScale, 19 * GuiHelper::contentScale)))
	{
		const std::string picked = fileHandler->openFile({{"All files", "*"}});

		if (!picked.empty())
			settings.file = picked;
	}

	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::HelpMarker("Selected file path replaces the {file} macro in the flash command below.");

	GuiHelper::drawTextAlignedToSize("Use *.elf/*.axf file:", alignment);
	ImGui::SameLine();
	ImGui::Checkbox("##useElfFile", &settings.useElfFile);
	ImGui::SameLine();
	ImGui::HelpMarker("When checked, {file} is replaced with the *.elf/*.axf path set in Options -> Acquisition.");

	/* FLASH COMMAND */
	GuiHelper::drawTextAlignedToSize("Flash command:", alignment);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##flashingCommand", &settings.command, 0, NULL, NULL);

	ImGui::TextDisabled("The path replaces every {file} in the command. Without one it is appended at the end.");

	/* TIMEOUT */
	GuiHelper::drawTextAlignedToSize("Timeout [s]:", alignment);
	ImGui::SameLine();
	ImGui::Checkbox("##flashingTimeoutEnabled", &settings.timeoutEnabled);
	ImGui::SameLine();

	ImGui::BeginDisabled(!settings.timeoutEnabled);
	ImGui::SetNextItemWidth(120 * GuiHelper::contentScale);

	int timeout = static_cast<int>(settings.timeoutSeconds);

	if (ImGui::DragInt("##flashingTimeoutSeconds", &timeout, 1.0f, FlashingService::minimumTimeoutSeconds, FlashingService::maximumTimeoutSeconds, "%d", ImGuiSliderFlags_AlwaysClamp))
		settings.timeoutSeconds = static_cast<int32_t>(timeout);

	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::HelpMarker("When enabled, flashing will be aborted automatically if there is no output for the specified time.");

	/* TEST */
	ImGui::Dummy(ImVec2(-1, 5));
	GuiHelper::drawCenteredText("Test flash");
	ImGui::Separator();

	const bool running = flasher->isRunning();

	ImGui::BeginDisabled(running);

	if (ImGui::Button("Test flash", ImVec2(120 * GuiHelper::contentScale, 25 * GuiHelper::contentScale)))
		startFlash(true);

	ImGui::EndDisabled();

	ImGui::SameLine();

	if (running)
	{
		if (ImGui::Button("Abort##testFlashAbort", ImVec2(120 * GuiHelper::contentScale, 25 * GuiHelper::contentScale)))
			flasher->abort();
	}

	ImGui::TextUnformatted("Test output:");

	ImGui::BeginChild("##testFlashOutput", ImVec2(-1, 150 * GuiHelper::contentScale), true);

	if (lastFlashKind != FlashKind::Test)
	{
		/* Nothing has been tried from this window, so there is nothing to show
		   even if a flash the user started elsewhere has printed something. */
		ImGui::TextDisabled("Not tested");
		ImGui::TextDisabled("Click the button above to test the flash command.");
	}
	else
	{
		const std::vector<std::string>& lines = flashOutput();

		if (lines.empty())
			ImGui::TextDisabled("(no output)");
		else
			for (const std::string& line : lines)
				ImGui::TextUnformatted(line.c_str());
	}

	ImGui::EndChild();

	const float buttonHeight = 25.0f * GuiHelper::contentScale;
	ImGui::SetCursorPos(ImVec2(0, ImGui::GetWindowSize().y - buttonHeight / 2.0f - ImGui::GetFrameHeightWithSpacing()));

	if (ImGui::Button("Done", ImVec2(-1, buttonHeight)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		showFlashingSettingsWindow = false;
		globalConfig->save();
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}

void Gui::drawWritePlannerWindow()
{
	writePlannerWindow->setShowWindowState(showWritePlannerWindow);
	writePlannerWindow->draw();
	showWritePlannerWindow = writePlannerWindow->getShowWindowState();
}
