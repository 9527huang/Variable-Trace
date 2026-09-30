#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Gui.hpp"
#include "PngWriter.hpp"
#include "glfw3.h"

/*
 * File -> Save Plots to *.png.
 *
 * The image is taken from the window framebuffer rather than from a rendering
 * pass of its own. What ends up in the file is therefore what the plot looked
 * like on screen, which is what the person pressing the shortcut was looking
 * at. The trade is that a plot scrolled out of view cannot be captured: its
 * rectangle is trimmed to the part that is on screen.
 *
 * The pixels have to be read between the frame being rendered and the buffers
 * being swapped. Anywhere else either reads the frame before this one or finds
 * the back buffer already gone. That moment also comes before a window can be
 * shown, so the work runs in two steps: the frame is taken first, and the names
 * are settled afterwards, at the one point where the number of images is known
 * and none of the files exist yet.
 */

namespace
{
	const char* const cancelledMessage = "Plot image save canceled.";

	const char* const nullHandlerMessage = "Failed to save plot image: file handler is not available for Save As dialog.";
	const char* const invalidDataMessage = "Failed to save plot image: invalid screenshot data returned by renderer.";
	const char* const nothingDrawnMessage = "No visible plot to save.";

	const char* const emptyDirectoryHint = "Empty means the directory the program was started from.";
	const char* const repeatedNameWarning =
		"Increment file name is off, so every plot is written to the same file and the last one is the one that survives.";

	/* How many rows of the target list the dialog prints before it says how
	   many are left over. A run of twenty plots is rare, and printing all of
	   them would push the buttons off the bottom of the dialog. */
	const size_t maxTargetRowsShown = 6;

	/* OpenGL hands the rows over from the bottom of the image upwards, and a
	   PNG holds them from the top down. */
	void flipRows(uint8_t* pixels, int width, int height)
	{
		const size_t stride = static_cast<size_t>(width) * 4;
		std::vector<uint8_t> row(stride);

		for (int y = 0; y < height / 2; y++)
		{
			uint8_t* top = pixels + static_cast<size_t>(y) * stride;
			uint8_t* bottom = pixels + static_cast<size_t>(height - 1 - y) * stride;

			std::copy(top, top + stride, row.begin());
			std::copy(bottom, bottom + stride, top);
			std::copy(row.begin(), row.end(), bottom);
		}
	}
}

void Gui::requestPlotImages()
{
	/* A capture can only be taken with no dialog on screen, because the
	   dimming of a modal would end up in the picture. The shortcut is read
	   every frame and does not stop at a popup the way a menu entry does, so
	   the refusal lives here. */
	if (plotExportDialogOpen || !pendingPlotImages.empty())
		return;

	plotImagesRequested = true;
}

void Gui::recordDrawnPlot(const std::string& name)
{
	/* The rectangle comes from ImGui rather than from ImPlot::GetPlotPos and
	   GetPlotSize: those two call SetupLock() on the way out, which closes the
	   setup stage of the plot they are asked about. Every plot calls SetupAxis
	   right after this, and a setup call after the lock is closed trips an
	   assertion inside ImPlot. ImGui already holds the same rectangle, because
	   BeginPlot registers the whole plot frame as the item it just added, and
	   that frame is the more useful picture anyway: it carries the title and
	   the axis labels that the inner plot rectangle leaves out. */
	recordDrawnPlot(name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

void Gui::recordDrawnPlot(const std::string& name, const ImVec2& min, const ImVec2& max)
{
	/* Called from inside every plot of every frame, so the first thing it does
	   is leave again unless somebody asked for the images. */
	if (!plotImagesRequested)
		return;

	/* A window that has been squeezed down to nothing still reports a plot,
	   with a rectangle that holds no pixels. Keeping it would name a file
	   after a plot nobody can see, so it is dropped here, before it is
	   counted. */
	if (max.x <= min.x || max.y <= min.y)
		return;

	drawnPlots.push_back({name, min, max});
}

bool Gui::readFramebufferRegion(const ImVec2& min, const ImVec2& max, int& width, int& height, std::vector<uint8_t>& rgba)
{
	const ImGuiIO& io = ImGui::GetIO();

	/* Interface coordinates are the ones the window reports, the framebuffer
	   counts device pixels. The backend keeps both, and the ratio between them
	   is what maps one onto the other. */
	const ImVec2 scale = io.DisplayFramebufferScale;

	const int framebufferWidth = static_cast<int>(io.DisplaySize.x * scale.x);
	const int framebufferHeight = static_cast<int>(io.DisplaySize.y * scale.y);

	if (framebufferWidth <= 0 || framebufferHeight <= 0)
		return false;

	int left = static_cast<int>(std::floor(min.x * scale.x));
	int top = static_cast<int>(std::floor(min.y * scale.y));
	int right = static_cast<int>(std::ceil(max.x * scale.x));
	int bottom = static_cast<int>(std::ceil(max.y * scale.y));

	left = std::clamp(left, 0, framebufferWidth);
	right = std::clamp(right, 0, framebufferWidth);
	top = std::clamp(top, 0, framebufferHeight);
	bottom = std::clamp(bottom, 0, framebufferHeight);

	width = right - left;
	height = bottom - top;

	if (width <= 0 || height <= 0)
		return false;

	rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

	glPixelStorei(GL_PACK_ALIGNMENT, 1);

	/* The row to read from is counted from the bottom of the framebuffer, so
	   the lower edge of the rectangle is what has to be converted. */
	glReadPixels(left, framebufferHeight - bottom, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

	flipRows(rgba.data(), width, height);

	return true;
}

Gui::SaveOutcome Gui::savePlotImage(const PlotImage& image, const plotExport::Settings& settings, size_t index, size_t count, std::string& error)
{
	/* The capture refuses a rectangle that holds no pixels, so reaching this
	   with one means the data was lost on the way here rather than missing
	   from the screen. */
	if (image.width <= 0 || image.height <= 0 || image.rgba.empty())
	{
		error = invalidDataMessage;
		return SaveOutcome::Failed;
	}

	std::string path = plotExport::imagePath(settings, index, count);

	if (settings.askForLocation)
	{
		if (fileHandler == nullptr)
		{
			error = nullHandlerMessage;
			return SaveOutcome::Failed;
		}

		/* The export directory is where the dialog starts rather than where
		   the file lands, so the name is built on its own and the dialog
		   returns the whole path. */
		path = fileHandler->saveFile({{"PNG image (*.png)", "png"}}, settings.directory, plotExport::imageFileName(settings, index, count));

		/* The file handler answers a dismissed dialog and one that could not
		   be opened with the same empty path, so the two share one message. */
		if (path.empty())
			return SaveOutcome::Cancelled;

		/* A name typed without an extension still means a picture, because that
		   is the only thing this dialog offers. */
		path = plotExport::withPngExtension(path);
	}

	if (!pngWriter::write(path, image.width, image.height, image.rgba.data()))
	{
		error = std::string("Failed to save plot image to: ") + path;
		return SaveOutcome::Failed;
	}

	logger->info("Saved plot image to: {}", path);

	return SaveOutcome::Written;
}

bool Gui::writePlotImages()
{
	const plotExport::Settings& settings = globalConfig->getSettings().plotExport;
	const size_t count = pendingPlotImages.size();

	size_t written = 0;
	size_t cancelled = 0;
	std::string firstError;

	for (size_t index = 0; index < count; index++)
	{
		std::string error;

		switch (savePlotImage(pendingPlotImages[index], settings, index, count, error))
		{
			case SaveOutcome::Written:
				written++;
				break;
			case SaveOutcome::Cancelled:
				cancelled++;
				break;
			case SaveOutcome::Failed:
				logger->error("{}", error);

				if (firstError.empty())
					firstError = error;
				break;
		}
	}

	if (cancelled > 0 && written == 0 && firstError.empty())
		logger->info("{}", cancelledMessage);

	if (!firstError.empty())
	{
		/* The pixels are still in hand, so a path that could not be written
		   costs one correction rather than a fresh capture. */
		plotExportError = firstError;
		return false;
	}

	pendingPlotImages.clear();
	plotExportError.clear();

	if (written > 0)
	{
		pendingPlotImageTitle = "Info";
		pendingPlotImageMessage = "Saved " + std::to_string(written) + (written == 1 ? " plot image." : " plot images.");
		pendingPlotImageSeconds = 2.0f;
	}

	return true;
}

void Gui::capturePlotImages()
{
	if (!plotImagesRequested)
		return;

	plotImagesRequested = false;

	/* The rectangles belong to the frame that has just been consumed, and the
	   pixels can only be read right now, so both lists are remade here. */
	pendingPlotImages.clear();

	for (const DrawnPlot& plot : drawnPlots)
	{
		PlotImage image;
		image.name = plot.name;

		if (readFramebufferRegion(plot.min, plot.max, image.width, image.height, image.rgba))
			pendingPlotImages.push_back(std::move(image));
	}

	drawnPlots.clear();

	/* No rectangle survived the capture, which covers the three cases of no
	   plot being drawn, every plot being off screen, and the window holding no
	   pixels at all. */
	if (pendingPlotImages.empty())
	{
		logger->warn("No plot was drawn, so there is nothing to save");
		pendingPlotImageTitle = "Warning";
		pendingPlotImageMessage = nothingDrawnMessage;
		pendingPlotImageSeconds = 2.0f;
		return;
	}

	const plotExport::Settings& settings = globalConfig->getSettings().plotExport;

	/* The per image dialog already asks where each file goes and what it is
	   called, so it is the whole of the naming step and the batch dialog would
	   have nothing left to add but a second click. */
	if (settings.askForLocation)
	{
		if (!writePlotImages())
		{
			pendingPlotImageTitle = "Error!";
			pendingPlotImageMessage = plotExportError;
			pendingPlotImageSeconds = 3.0f;
			plotExportError.clear();
			pendingPlotImages.clear();
		}

		return;
	}

	plotExportError.clear();
	plotExportDialogOpen = true;
	plotExportDialogFocusName = true;
}

void Gui::drawPlotExportDialog()
{
	if (plotExportDialogOpen)
		ImGui::OpenPopup("Save Plots");

	plotExport::Settings& settings = globalConfig->getSettings().plotExport;

	/* The wrap position is absolute so the width of the dialog does not follow
	   the length of the paths it prints, which would make the dialog jump
	   sideways as the name is typed. */
	const float wrapWidth = 520.0f * GuiHelper::contentScale;
	const float fieldWidth = 360.0f * GuiHelper::contentScale;

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(wrapWidth + 40.0f * GuiHelper::contentScale, 0.0f), ImVec2(FLT_MAX, FLT_MAX));

	if (ImGui::BeginPopupModal("Save Plots", &plotExportDialogOpen, ImGuiWindowFlags_AlwaysAutoResize))
	{
		const size_t count = pendingPlotImages.size();

		ImGui::Text("%zu %s to save", count, count == 1 ? "visible plot" : "visible plots");

		ImGui::SetNextItemWidth(fieldWidth);
		ImGui::InputText("Export directory:##screenshotDialogDirectory", &settings.directory, 0, NULL, NULL);
		ImGui::SameLine();

		if (ImGui::Button("...##screenshotDialogSelect"))
		{
			const std::string directory = fileHandler->openDirectory({});

			if (!directory.empty())
				settings.directory = directory;
		}

		if (settings.directory.empty())
			ImGui::TextDisabled("%s", emptyDirectoryHint);

		ImGui::SetNextItemWidth(fieldWidth);

		/* The name is what most people come here to change, so it takes the
		   keyboard as the dialog appears. With the whole of the old name
		   selected, the answer is often just typing a new one and pressing
		   Enter: the first character typed replaces what was there. */
		if (plotExportDialogFocusName)
		{
			plotExportDialogFocusName = false;
			ImGui::SetKeyboardFocusHere();
		}

		/* Enter in the name field is the whole of the answer most of the time,
		   so it saves rather than doing nothing. */
		const bool entered = ImGui::InputText("File name:##screenshotDialogFileName", &settings.fileName, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll, NULL, NULL);

		ImGui::SameLine();
		ImGui::HelpMarker("The *.png extension is added when it is missing.");

		ImGui::Checkbox("Increment file name:##screenshotDialogIncrement", &settings.incrementFileName);
		ImGui::SameLine();
		ImGui::HelpMarker("With several plots, numbers each file so none is written over.");

		ImGui::Separator();

		const size_t shown = std::min(count, maxTargetRowsShown);

		if (ImGui::BeginTable("##screenshotTargets", 2, ImGuiTableFlags_SizingFixedFit))
		{
			for (size_t index = 0; index < shown; index++)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(pendingPlotImages[index].name.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(plotExport::imageFileName(settings, index, count).c_str());
			}

			ImGui::EndTable();
		}

		if (count > shown)
			ImGui::TextDisabled("and %zu more", count - shown);

		/* The warning and the error are both long, and the error carries a
		   whole path. Left to themselves they stretch the dialog to fit, which
		   slides the buttons sideways at the moment they are about to be
		   pressed. Wrapping holds the dialog at the size it already has and
		   still shows every character of the text. */
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);

		/* A run that writes every plot to one name is legal and occasionally
		   what somebody wants, so it is allowed and said out loud. */
		if (!settings.incrementFileName && count > 1)
			ImGui::TextColored(GuiHelper::orangeLight, "%s", repeatedNameWarning);

		if (!plotExportError.empty())
			ImGui::TextColored(GuiHelper::redLight, "%s", plotExportError.c_str());

		ImGui::PopTextWrapPos();

		ImGui::Separator();

		const float buttonWidth = 120.0f * GuiHelper::contentScale;

		if (ImGui::Button("Save", ImVec2(buttonWidth, 0)) || entered)
		{
			if (writePlotImages())
			{
				plotExportDialogOpen = false;
				ImGui::CloseCurrentPopup();
			}
		}

		ImGui::SameLine();

		if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			/* Cancelling drops the capture rather than keeping it: the plots
			   have moved on since it was taken, and what is on screen now is
			   what the next press will ask for. */
			pendingPlotImages.clear();
			plotExportError.clear();
			plotExportDialogOpen = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
	else if (!plotExportDialogOpen && !pendingPlotImages.empty())
	{
		/* Closed with the cross of the dialog, which is the third way of
		   saying no. */
		pendingPlotImages.clear();
		plotExportError.clear();
	}
}

void Gui::drawPlotExportSettings()
{
	plotExport::Settings& settings = globalConfig->getSettings().plotExport;

	ImGui::TextUnformatted("Plot export");

	ImGui::Checkbox("Ask for location:##askForScreenshotLocation", &settings.askForLocation);
	ImGui::SameLine();
	ImGui::HelpMarker("When enabled, each screenshot opens a Save As dialog of its own and the two fields below are only what it starts from.");

	/* These are the values the export dialog opens with, so they stay editable
	   in both modes. */
	ImGui::SetNextItemWidth(300 * GuiHelper::contentScale);
	ImGui::InputText("Export directory:##screenshotDirectory", &settings.directory, 0, NULL, NULL);
	ImGui::SameLine();

	if (ImGui::Button("...##screenshotSelect"))
	{
		const std::string directory = fileHandler->openDirectory({});

		if (!directory.empty())
			settings.directory = directory;
	}

	if (settings.directory.empty())
		ImGui::TextDisabled("%s", emptyDirectoryHint);

	ImGui::SetNextItemWidth(300 * GuiHelper::contentScale);
	ImGui::InputText("File name:##screenshotFileName", &settings.fileName, 0, NULL, NULL);
	ImGui::SameLine();
	ImGui::HelpMarker("The *.png extension is added when it is missing.");

	ImGui::Checkbox("Increment file name:##incrementFileName", &settings.incrementFileName);
	ImGui::SameLine();
	ImGui::HelpMarker("With several plots, numbers each file so none is written over.");
}
