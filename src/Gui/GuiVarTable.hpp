#pragma once

#include <filesystem>
#include <future>
#include <set>
#include <string>

#include "ElfParserFactory.hpp"
#include "GuiHelper.hpp"
#include "GuiImportVariables.hpp"
#include "GuiVariablesEdit.hpp"
#include "I18n.hpp"
#include "IElfParser.hpp"
#include "PlotHandler.hpp"
#include "Popup.hpp"
#include "Variable.hpp"
#include "VariableHandler.hpp"
#include "ViewerDataHandler.hpp"

class VariableTableWindow
{
   public:
	VariableTableWindow(ViewerDataHandler* viewerDataHandler, PlotHandler* plotHandler, VariableHandler* variableHandler, std::string* projectElfPath, std::string* projectConfigPath, spdlog::logger* logger) : viewerDataHandler(viewerDataHandler), plotHandler(plotHandler), variableHandler(variableHandler), projectElfPath(projectElfPath), projectConfigPath(projectConfigPath), logger(logger)
	{
		parser = ElfParserFactory::create(IElfParser::Type::Gdb, variableHandler, logger);
		parserName = parser->getName();
		variableEditWindow = std::make_shared<VariableEditWindow>(variableHandler);
		importVariablesWindow = std::make_shared<ImportVariablesWindow>(parser.get(), projectElfPath, projectConfigPath, variableHandler);
	}

	/* An address refresh made from the API is the same operation as this window
	   performs, so it has to move the same baseline. Otherwise the window keeps
	   offering to reload changes that were already applied. */
	void markElfRefreshed()
	{
		lastModifiedTime = std::filesystem::file_time_type::clock::now();
	}

	/* Follows the setting. A change of parser replaces the object the table
	   parses with, because the names and the addresses it produced came from a
	   different reading of the file and cannot be carried across. */
	void syncParser()
	{
		const ViewerDataHandler::Settings settings = viewerDataHandler->getSettings();

		/* A refresh already running holds the parser it started on. Swapping
		   now would let the old one finish into an object nobody reads from,
		   and the new one would never see the file. The change is picked up on
		   the next frame instead. */
		if (settings.elfParser != parserName && !isRefreshRunning())
		{
			parser = ElfParserFactory::create(settings.elfParser, variableHandler, logger);
			parserName = parser->getName();

			if (importVariablesWindow != nullptr)
				importVariablesWindow->setParser(parser.get());

			/* The addresses in the table were read by the previous parser, so
			   the file counts as unread by this one. */
			markElfRefreshed();
		}

		ElfParserFactory::applySettings(*parser, settings.gdbCommand, settings.ofd2000Command);
	}

	void draw()
	{
		static ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable;
		static std::set<std::string> selection;

		syncParser();

		/* Nothing here is held back while acquisition runs. Adding, deleting and
		   renaming a variable are all changes to the model the acquisition
		   thread reads, and VariableHandler makes each of them one indivisible
		   step under a lock the acquisition thread takes as well. The rest of
		   this pane only reads, and the two that do change what is sampled -
		   the address refresh and a variable going onto a plot - ask for the
		   list of addresses to be rebuilt. */
		ImGui::Dummy(ImVec2(-1, 5));
		GuiHelper::drawCenteredText(I18n::tr("Variables"));
		ImGui::SameLine();
		ImGui::HelpMarker(I18n::tr("Select your *.elf/*.axf file in the Options->Acqusition Settings to import or update the variables."));
		ImGui::Separator();

		drawAddVariableButton();
		drawUpdateAddressesFromElf();

		/* Padded to the width the English label takes, so that the field beside
		   it starts in the same place in either language. */
		const std::string label = GuiHelper::padToColumn(I18n::tr("search"), 7);
		ImGui::PushItemWidth(ImGui::GetItemRectSize().x - ImGui::CalcTextSize(label.c_str()).x - 8 * GuiHelper::contentScale);
		static std::string search{};
		ImGui::Text("%s", label.c_str());
		ImGui::SameLine();
		ImGui::InputText("##search", &search, 0, NULL, NULL);
		ImGui::PopItemWidth();

		if (ImGui::BeginTable("table_scrolly", 3, flags, ImVec2(0.0f, 300 * GuiHelper::contentScale)))
		{
			ImGui::TableSetupScrollFreeze(0, 1);

			/* The header is the translation, but the id the column width is
			   saved under is taken from the English, so that switching language
			   does not throw the widths away. */
			ImGui::TableSetupColumn(I18n::tr("Name"), 0, 0.0f, ImGui::GetID("Name"));
			ImGui::TableSetupColumn(I18n::tr("Address"), 0, 0.0f, ImGui::GetID("Address"));
			ImGui::TableSetupColumn(I18n::tr("Type"), 0, 0.0f, ImGui::GetID("Type"));
			ImGui::TableHeadersRow();

			std::optional<std::string> varNameToDelete;
			std::string currentName{};

			for (std::shared_ptr<Variable> var : *variableHandler)
			{
				std::string name = var->getName();
				if (toLower(name).find(toLower(search)) == std::string::npos)
					continue;

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::PushID(name.c_str());
				ImGui::ColorEdit4("##", &var->getColor().r, ImGuiColorEditFlags_NoInputs);
				ImGui::SameLine();
				ImGui::PopID();

				const bool itemIsSelected = selection.contains(name);

				if (ImGui::Selectable(var->getName().c_str(), itemIsSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap | ImGuiSelectableFlags_AllowDoubleClick))
				{
					if (ImGui::IsMouseDoubleClicked(0))
					{
						variableEditWindow->setVariableToEdit(var);
						variableEditWindow->setShowVariableEditWindowState(true);
					}

					if (ImGui::GetIO().KeyCtrl && var->getIsFound())
					{
						if (itemIsSelected)
							selection.erase(name);
						else
							selection.insert(name);
					}
					else
						selection.clear();
				}

				drawMenuVariablePopup(name, [&]()
									  { variableHandler->addNewVariable(""); }, [&](std::string name)
									  { variableHandler->addNewVariable(name); }, [&](std::string name)
									  { varNameToDelete = name; }, [&](std::string name)
									  {	variableEditWindow->setVariableToEdit(var);
                                                                    variableEditWindow->setShowVariableEditWindowState(true); });

				if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
				{
					if (selection.empty())
						selection.insert(name);

					/* pass a pointer to the selection as we have to call clear() on the original object upon receiving */
					std::set<std::string>* selectionPtr = &selection;
					ImGui::SetDragDropPayload("MY_DND", &selectionPtr, sizeof(selectionPtr));
					ImGui::PushID(name.c_str());
					ImGui::ColorEdit4("##", &variableHandler->getVariable(*selection.begin())->getColor().r, ImGuiColorEditFlags_NoInputs);
					ImGui::SameLine();
					ImGui::PopID();

					if (selection.size() > 1)
						ImGui::TextUnformatted(I18n::tr("<multiple vars>"));
					else
						ImGui::TextUnformatted(selection.begin()->c_str());
					ImGui::EndDragDropSource();
				}
				ImGui::TableSetColumnIndex(1);

				/* A derived variable has no address, so the column says which
				   it is rather than a zero. A zero there reads as an address
				   that happens to be invalid, and this one is not an address
				   at all. */
				if (var->isDerived())
					ImGui::TextDisabled("%s", I18n::tr("(derived)"));
				else if (var->getIsFound())
					ImGui::Text("%s", ("0x" + std::string(GuiHelper::intToHexString(var->getAddress()))).c_str());
				else
					ImGui::TextColored(GuiHelper::redLight(), I18n::tr("NOT FOUND!"));

				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%s", var->getTypeStr().c_str());
			}

			if (varNameToDelete.has_value())
			{
				/* The editor holds its variable alive by itself, so it is told
				   first: otherwise it would keep editing an object that no
				   longer belongs to the collection and every field it writes
				   would go nowhere. */
				variableEditWindow->forgetVariable(varNameToDelete.value());

				/* Takes the series that pointed at it off every plot and then
				   drops it, as one step. */
				variableHandler->erase(varNameToDelete.value());

				/* What is read from the target is built from the plots, so a
				   variable that has just left them has to ask for a rebuild.
				   Ignored while stopped. */
				viewerDataHandler->requestRestart();
			}
			ImGui::EndTable();
		}

		variableEditWindow->draw();
		importVariablesWindow->draw();
	}

   private:
	void drawAddVariableButton()
	{
		if (ImGui::Button(I18n::tr("Add variable"), ImVec2(-1, 25 * GuiHelper::contentScale)))
		{
			variableHandler->addNewVariable("");
		}

		ImGui::BeginDisabled(projectElfPath->empty());

		if (ImGui::Button(I18n::tr("Import variables from *.elf/*.axf"), ImVec2(-1, 25 * GuiHelper::contentScale)))
			importVariablesWindow->setShowImportVariablesWindow(true);

		ImGui::EndDisabled();
	}

	void drawUpdateAddressesFromElf()
	{
		static bool shouldPopStyle = false;

		/* Wide enough for the longest label in either language: a translated
		   label is longer in bytes than the English it replaces, and this one
		   carries a file pattern inside it. */
		static constexpr size_t textSize = 80;
		char buttonText[textSize]{};

		const bool refreshing = isRefreshRunning();

		if (refreshing)
		{
			refreshWasRunning = true;
			snprintf(buttonText, textSize, "%s %c", I18n::tr("Update variable addresses"), "|/-\\"[(int)(ImGui::GetTime() / 0.05f) & 3]);
		}
		else
		{
			snprintf(buttonText, textSize, "%s", I18n::tr("Update variable addresses"));
			if (refreshThread.valid() && !refreshThread.get())
				popup.show(I18n::tr("Error!"), I18n::tr("Update error. Please check the *.elf/*.axf file path!"), 2.0f);

			/* The addresses were just rewritten, and the set of them that is
			   read from the target is built from the plots. Asking here rather
			   than inside the pass keeps the acquisition thread doing the
			   rebuild, which is where it already can do it without a race.
			   Ignored while stopped, where the next start builds the list. */
			if (refreshWasRunning)
			{
				refreshWasRunning = false;
				viewerDataHandler->requestRestart();
			}
		}

		ImGui::BeginDisabled(projectElfPath->empty());

		bool elfChanged = checkElfFileChanged();

		performVariablesUpdate = importVariablesWindow->shouldPerformVariableUpdate();

		if (elfChanged)
		{
			ImVec4 color = GuiHelper::orange();
			snprintf(buttonText, textSize, "%s", I18n::tr("Click to reload *.elf/*.axf changes!"));
			ImGui::PushStyleColor(ImGuiCol_Button, color);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);

			if (viewerDataHandler->getSettings().stopAcqusitionOnElfChange)
				viewerDataHandler->setState(DataHandlerBase::State::STOP);

			if (viewerDataHandler->getSettings().refreshAddressesOnElfChange)
			{
				performVariablesUpdate = true;
				/* TODO: examine why elf is not ready to be parsed without the delay */
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}

			shouldPopStyle = true;
		}

		if (ImGui::Button(buttonText, ImVec2(-1, 25 * GuiHelper::contentScale)) || performVariablesUpdate)
		{
			lastModifiedTime = std::filesystem::file_time_type::clock::now();
			refreshThread = std::async(std::launch::async, &IElfParser::updateVariableMap, parser, GuiHelper::convertProjectPathToAbsolute(projectElfPath, projectConfigPath));
			performVariablesUpdate = false;
		}

		/* TODO fix this ugly solution */
		if (shouldPopStyle)
		{
			ImGui::PopStyleColor(3);
			shouldPopStyle = false;
		}
		ImGui::EndDisabled();
	}

	bool isRefreshRunning()
	{
		return refreshThread.valid() && refreshThread.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
	}

	bool checkElfFileChanged()
	{
		std::string path = GuiHelper::convertProjectPathToAbsolute(projectElfPath, projectConfigPath);
		if (!std::filesystem::exists(path))
			return false;

		auto writeTime = std::filesystem::last_write_time(path);
		return writeTime > lastModifiedTime;
	}

	void drawMenuVariablePopup(const std::string& name, std::function<void()> onNew, std::function<void(const std::string&)> onCopy, std::function<void(const std::string&)> onDelete, std::function<void(const std::string&)> onProperties)
	{
		ImGui::PushID(name.c_str());
		if (ImGui::BeginPopupContextItem(name.c_str()))
		{
			if (ImGui::MenuItem(I18n::tr("New")))
				onNew();

			if (ImGui::MenuItem(I18n::tr("Copy")))
				onCopy(name);

			if (ImGui::MenuItem(I18n::tr("Delete")))
				onDelete(name);

			if (ImGui::MenuItem(I18n::tr("Properties")))
				onProperties(name);

			ImGui::EndPopup();
		}
		ImGui::PopID();
	}

   private:
	ViewerDataHandler* viewerDataHandler;
	PlotHandler* plotHandler;
	VariableHandler* variableHandler;
	std::string* projectElfPath;
	std::string* projectConfigPath;
	spdlog::logger* logger;

	Popup popup;
	std::shared_ptr<IElfParser> parser;
	/* The name the parser above was built from, so that a change of setting can
	   be noticed without keeping a second copy of the settings. */
	std::string parserName;
	std::shared_ptr<VariableEditWindow> variableEditWindow;
	std::shared_ptr<ImportVariablesWindow> importVariablesWindow;

	std::filesystem::file_time_type lastModifiedTime = std::filesystem::file_time_type::clock::now();

	/* Owned by the window rather than by the drawing function, because the
	   parser is replaced between frames and a refresh in flight must not be
	   mistaken for one on the new parser. */
	std::future<bool> refreshThread{};

	bool performVariablesUpdate = false;

	/* Whether the pass above was running when the last frame was drawn, so that
	   the frame it finishes on can be told apart from all the ones after it.
	   The future is read once when it completes and cannot be asked again. */
	bool refreshWasRunning = false;
};