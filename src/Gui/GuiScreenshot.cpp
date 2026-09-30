#include <algorithm>
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
 * The capture has to happen between the frame being rendered and the buffers
 * being swapped. Anywhere else either reads the frame before this one or finds
 * the back buffer already gone.
 */

namespace
{
	const char* const cancelledMessage = "Plot image save canceled.";

	const char* const nullHandlerMessage = "Failed to save plot image: file handler is not available for Save As dialog.";
	const char* const invalidDataMessage = "Failed to save plot image: invalid screenshot data returned by renderer.";
	const char* const nothingDrawnMessage = "No visible plot to save.";

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
	   with a rectangle that holds no pixels. Keeping it would write a file
	   named after a plot nobody can see and report a failure for it, so it is
	   dropped here instead, before it is counted. */
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

Gui::SaveOutcome Gui::savePlotImage(const DrawnPlot& plot, const plotExport::Settings& settings, size_t index, size_t count, std::string& error)
{
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgba;

	/* Nothing on screen means nothing to read: the plot was scrolled out of
	   the window, or the window holds no pixels at all. */
	if (!readFramebufferRegion(plot.min, plot.max, width, height, rgba))
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

	if (!pngWriter::write(path, width, height, rgba.data()))
	{
		error = std::string("Failed to save plot image to: ") + path;
		return SaveOutcome::Failed;
	}

	logger->info("Saved plot image to: {}", path);

	return SaveOutcome::Written;
}

void Gui::processPlotImages()
{
	if (!plotImagesRequested)
		return;

	plotImagesRequested = false;

	const plotExport::Settings& settings = globalConfig->getSettings().plotExport;

	if (drawnPlots.empty())
	{
		logger->warn("No plot was drawn, so there is nothing to save");
		pendingPlotImageTitle = "Warning";
		pendingPlotImageMessage = nothingDrawnMessage;
		pendingPlotImageSeconds = 2.0f;
		return;
	}

	/* The count is taken before the first dialog is opened, so a run that is
	   answered half way through still numbers its files the way it started. */
	const size_t count = drawnPlots.size();
	size_t written = 0;
	size_t cancelled = 0;
	std::string firstError;

	for (size_t index = 0; index < count; index++)
	{
		std::string error;

		switch (savePlotImage(drawnPlots[index], settings, index, count, error))
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

	/* Someone who dismissed every dialog asked for nothing, and is told
	   nothing. A failure is the case that has to reach them. */
	if (!firstError.empty())
	{
		pendingPlotImageTitle = "Error!";
		pendingPlotImageMessage = firstError;
		pendingPlotImageSeconds = 3.0f;
	}

	/* The rectangles belong to the frame that has just been consumed. The next
	   frame fills the list again. */
	drawnPlots.clear();
}

void Gui::drawPlotExportSettings()
{
	plotExport::Settings& settings = globalConfig->getSettings().plotExport;

	ImGui::TextUnformatted("Plot export");

	ImGui::Checkbox("Ask for location:##askForScreenshotLocation", &settings.askForLocation);
	ImGui::SameLine();
	ImGui::HelpMarker("When enabled, each screenshot will open a Save As dialog.");

	/* Without the dialog the name and the directory below decide the path, so
	   they are only editable while it is off. */
	ImGui::BeginDisabled(settings.askForLocation);

	ImGui::SetNextItemWidth(300 * GuiHelper::contentScale);
	ImGui::InputText("Export directory:##screenshotDirectory", &settings.directory, 0, NULL, NULL);
	ImGui::SameLine();

	if (ImGui::Button("...##screenshotSelect"))
	{
		const std::string directory = fileHandler->openDirectory({});

		if (!directory.empty())
			settings.directory = directory;
	}

	ImGui::SetNextItemWidth(300 * GuiHelper::contentScale);
	ImGui::InputText("File name:##screenshotFileName", &settings.fileName, 0, NULL, NULL);
	ImGui::SameLine();
	ImGui::HelpMarker("The *.png extension is added when it is missing.");

	ImGui::Checkbox("Increment file name:##incrementFileName", &settings.incrementFileName);
	ImGui::SameLine();
	ImGui::HelpMarker("Increment the file name when saving multiple plots.");

	ImGui::EndDisabled();
}
