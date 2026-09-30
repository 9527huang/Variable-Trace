#ifndef _PLOTEXPORT_HPP
#define _PLOTEXPORT_HPP

#include <cstddef>
#include <string>
#include <vector>

/*
 * The naming and path rules behind File -> Save Plots to *.png.
 *
 * Kept apart from the capture so the rules can be tested without an OpenGL
 * context: everything here is string work.
 */
namespace plotExport
{
	/* Where the images go and what they are called. Stored in the global config
	   rather than in the project file, because an export directory belongs to
	   the machine that is doing the debugging. */
	struct Settings
	{
		/* When set, every image opens a Save As dialog and the directory below
		   is only where that dialog starts. */
		bool askForLocation = false;
		std::string directory = "";
		std::string fileName = "MCUViewer_screenshot";
		/* Off lets every image of one run write over the same file. */
		bool incrementFileName = true;
	};

	/* Used when the configured name holds nothing that can be a file name. */
	extern const char* const defaultFileName;

	/* Replaces the characters a file name may not hold, drops the leading and
	   trailing ones Windows would silently trim, and falls back to
	   defaultFileName when nothing usable is left. */
	std::string sanitizeFileName(const std::string& name);

	/* Appends ".png" unless the name already carries that extension, in any
	   case. */
	std::string withPngExtension(const std::string& name);

	/* The name of the index-th image of a run that writes `count` of them.
	 *
	 * A run of one image keeps the plain name however many digits a longer run
	 * would need; that is the everyday case and it reads better in a listing.
	 * A longer run is numbered from one so the files sort in the order they
	 * were written. With incrementFileName off they all share one name and the
	 * last one written is the one that survives.
	 */
	std::string imageFileName(const Settings& settings, size_t index, size_t count);

	/* Joins a directory and a file name, accepting either separator in the
	   directory and tolerating an empty one. */
	std::string joinPath(const std::string& directory, const std::string& fileName);

	/* The full path of one image of a run, ready for the writer. */
	std::string imagePath(const Settings& settings, size_t index, size_t count);

	/* The paths a run of `count` images will write, for the export dialog to
	   show before any file exists.
	 *
	 * At most two are returned. A longer run numbers its files in one pattern,
	 * so the first two carry the whole meaning and the caller says how many
	 * there are; listing all of them would grow the dialog with the plot count
	 * and tell the reader nothing new. An empty result means there is nothing
	 * to write. */
	std::vector<std::string> targetPaths(const Settings& settings, size_t count);
}

#endif
