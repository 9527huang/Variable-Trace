#include "PngWriter.hpp"

/* The export directory is typed by hand and may hold characters outside ASCII,
   which fopen would not open on Windows. The library turns the path into UTF-16
   and calls the wide entry point instead. */
#define STBIW_WINDOWS_UTF8

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

bool pngWriter::write(const std::string& path, int width, int height, const uint8_t* rgba)
{
	if (path.empty() || rgba == nullptr || width <= 0 || height <= 0)
		return false;

	/* Four channels, and a stride of four bytes per pixel: the buffer the
	   capture produces is tightly packed. */
	return stbi_write_png(path.c_str(), width, height, 4, rgba, width * 4) != 0;
}
