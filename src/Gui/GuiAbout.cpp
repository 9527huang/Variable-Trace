#include "../commons.hpp"
#include "../gitversion.hpp"
#include "Gui.hpp"
#include "I18n.hpp"

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <shellapi.h>
#endif

void Gui::drawAboutWindow()
{
	if (showAboutWindow)
		ImGui::OpenPopup(I18n::windowTitle("About").c_str());

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(500 * GuiHelper::contentScale, 300 * GuiHelper::contentScale));
	if (ImGui::BeginPopupModal(I18n::windowTitle("About").c_str(), &showAboutWindow, 0))
	{
		GuiHelper::drawCenteredText(I18n::tr("Variable-Trace"));
		std::string line2(std::string(I18n::tr("version: ")) + std::to_string(VARIABLE_TRACE_VERSION_MAJOR) + "." + std::to_string(VARIABLE_TRACE_VERSION_MINOR) + "." + std::to_string(VARIABLE_TRACE_VERSION_REVISION));
		GuiHelper::drawCenteredText(std::move(line2));
		GuiHelper::drawCenteredText(std::string(GIT_HASH));
		ImGui::SameLine();
		const bool copy = ImGui::SmallButton(I18n::tr("copy"));
		if (copy)
		{
			ImGui::LogToClipboard();
			ImGui::LogText("%s", GIT_HASH);
			ImGui::LogFinish();
		}

		ImGui::Dummy(ImVec2(-1, 18 * GuiHelper::contentScale));
		GuiHelper::drawCenteredText("LEI HUANG");
		GuiHelper::drawCenteredText("2583246961@qq.com");
		ImGui::Dummy(ImVec2(-1, 14 * GuiHelper::contentScale));

		/* This line is not a credit but a statement of what the build is: a
		   modified version of another work, carried under the licence that work
		   was released with. The licence asks a modified version to say so, so
		   the line stays even though the original author's name has been taken
		   out of the box. */
		GuiHelper::drawCenteredText(I18n::tr("a modified version of MCUViewer 1.1.0, under the GPLv3"));
		ImGui::Dummy(ImVec2(-1, 16 * GuiHelper::contentScale));

		const float buttonHeight = 25.0f * GuiHelper::contentScale;
		const float buttonWidth = 100.0f * GuiHelper::contentScale;
		const float buttonsWidth = buttonWidth * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;

		ImGui::SetCursorPosX((ImGui::GetWindowSize().x - buttonsWidth) / 2.0f);

		if (ImGui::Button(I18n::tr("Releases"), ImVec2(buttonWidth, buttonHeight)))
			openWebsite("https://github.com/9527huang/Variable-Trace/releases");
		ImGui::SameLine();
		if (ImGui::Button(I18n::tr("Source"), ImVec2(buttonWidth, buttonHeight)))
			openWebsite("https://github.com/9527huang/Variable-Trace");
		ImGui::SameLine();
		if (ImGui::Button(I18n::tr("License"), ImVec2(buttonWidth, buttonHeight)))
			openWebsite("https://www.gnu.org/licenses/gpl-3.0.html");

		ImGui::SetCursorPos(ImVec2(0, ImGui::GetWindowSize().y - buttonHeight / 2.0f - ImGui::GetFrameHeightWithSpacing()));
		if (ImGui::Button(I18n::tr("Done"), ImVec2(-1, buttonHeight)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			showAboutWindow = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}

bool Gui::openWebsite(const char* url)
{
#ifdef _WIN32
	/* A returned value above 32 means the shell started something. Everything
	   at or below it is an error code, and the meanings those codes were given
	   are no longer useful, so they are reported as a plain failure. */
	const intptr_t result = reinterpret_cast<intptr_t>(ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL));
	return result > 32;
#elif defined(__APPLE__) || defined(_UNIX)
	const char* browser = getenv("BROWSER");
	if (browser == NULL)
		browser = "xdg-open";
	char command[256];
	snprintf(command, sizeof(command), "%s %s", browser, url);
	// auto status = system(command);
#else
#error "Your system is not supported!"
#endif
	return true;
}

/* Opens the user guide, a file placed beside the executable when the project is
   built. The path is worked out from the executable rather than from the
   working directory: the program can be started from a shortcut or from any
   other directory, and the working directory follows that rather than the
   program it started. */
void Gui::openUserGuide()
{
	std::string directory;

#ifdef _WIN32
	char path[MAX_PATH]{};
	const DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);

	if (length == 0 || length >= MAX_PATH)
	{
		logger->warn("The directory of the executable could not be read, so the user guide cannot be located.");
		return;
	}

	directory.assign(path, length);

	const size_t separator = directory.find_last_of("\\/");

	if (separator == std::string::npos)
	{
		logger->warn("No directory could be worked out of the executable path '{}'.", directory);
		return;
	}

	directory.resize(separator + 1);
#else
	/* Asking a running program where it lives is not portable, so on the other
	   platforms the guide is looked for beside the working directory. */
	directory = "./";
#endif

	const std::string guide = directory + "Variable-Trace-User-Guide.html";

	/* Said out loud rather than handed to the shell: the shell answers a
	   missing file with a dialog of its own, and that dialog says nothing
	   about which file was wanted. */
	if (!std::filesystem::exists(guide))
	{
		logger->warn("The user guide is not there (looked for '{}'). It is copied next to the executable when the project is built.", guide);
		return;
	}

	if (!openWebsite(guide.c_str()))
		logger->warn("The user guide at '{}' could not be handed to the shell to open.", guide);
	else
		logger->info("The user guide was opened: '{}'", guide);
}
