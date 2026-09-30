#ifndef _PNGWRITER_HPP
#define _PNGWRITER_HPP

#include <cstdint>
#include <string>

/*
 * The one place that turns a pixel buffer into a file.
 *
 * Holds the stb_image_write implementation, so this translation unit is the
 * only one that pays for compiling it. It takes no part in the capture, which
 * keeps it usable from a test that has no OpenGL context.
 */
namespace pngWriter
{
	/* Writes an 8 bit RGBA buffer, top row first, as a PNG.
	   Returns false when the arguments cannot describe an image or the file
	   could not be created. */
	bool write(const std::string& path, int width, int height, const uint8_t* rgba);
}

#endif
