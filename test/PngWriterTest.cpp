#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "PngWriter.hpp"

/*
 * The writer is the last step of an export, and the one that decides whether
 * anything at all reached the disk. It needs no window, so a real file can be
 * read back here rather than being taken on trust.
 */

namespace
{
	class PngWriterTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			directory = std::filesystem::temp_directory_path() / "variable-trace-png-writer-test";

			std::error_code errorCode;
			std::filesystem::remove_all(directory, errorCode);
			std::filesystem::create_directories(directory);

			path = (directory / "image.png").string();
		}

		void TearDown() override
		{
			std::error_code errorCode;
			std::filesystem::remove_all(directory, errorCode);
		}

		static std::vector<uint8_t> readFile(const std::string& file)
		{
			std::ifstream stream(file, std::ios::binary);

			return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		static uint32_t readBigEndian(const std::vector<uint8_t>& data, size_t offset)
		{
			return (static_cast<uint32_t>(data[offset]) << 24) | (static_cast<uint32_t>(data[offset + 1]) << 16) |
				   (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
		}

		std::filesystem::path directory;
		std::string path;
	};

	/* A small image with every pixel a different colour, so a buffer that was
	   read back in the wrong order would not go unnoticed. */
	std::vector<uint8_t> gradient(int width, int height)
	{
		std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

		for (int y = 0; y < height; y++)
		{
			for (int x = 0; x < width; x++)
			{
				const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
				pixels[index] = static_cast<uint8_t>(x * 7);
				pixels[index + 1] = static_cast<uint8_t>(y * 11);
				pixels[index + 2] = static_cast<uint8_t>(x + y);
				pixels[index + 3] = 255;
			}
		}

		return pixels;
	}
}

TEST_F(PngWriterTest, aWrittenFileIsAPng)
{
	const std::vector<uint8_t> pixels = gradient(4, 3);

	ASSERT_TRUE(pngWriter::write(path, 4, 3, pixels.data()));

	const std::vector<uint8_t> written = readFile(path);

	ASSERT_GT(written.size(), 8u);

	/* The eight byte signature every PNG starts with. */
	const std::vector<uint8_t> signature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

	EXPECT_TRUE(std::equal(signature.begin(), signature.end(), written.begin()));
}

TEST_F(PngWriterTest, theHeaderCarriesTheSizeItWasGiven)
{
	const std::vector<uint8_t> pixels = gradient(13, 7);

	ASSERT_TRUE(pngWriter::write(path, 13, 7, pixels.data()));

	const std::vector<uint8_t> written = readFile(path);

	ASSERT_GT(written.size(), 24u);

	/* Signature, then the length of the first chunk, then its type. The image
	   description follows immediately. */
	EXPECT_EQ(std::string(written.begin() + 12, written.begin() + 16), "IHDR");
	EXPECT_EQ(readBigEndian(written, 16), 13u);
	EXPECT_EQ(readBigEndian(written, 20), 7u);
}

TEST_F(PngWriterTest, aSinglePixelIsEnough)
{
	const std::vector<uint8_t> pixels = {10, 20, 30, 255};

	EXPECT_TRUE(pngWriter::write(path, 1, 1, pixels.data()));
	EXPECT_TRUE(std::filesystem::exists(path));
}

TEST_F(PngWriterTest, anExistingFileIsReplaced)
{
	const std::vector<uint8_t> first = gradient(2, 2);
	ASSERT_TRUE(pngWriter::write(path, 2, 2, first.data()));

	const uintmax_t smallSize = std::filesystem::file_size(path);

	const std::vector<uint8_t> second = gradient(16, 16);
	ASSERT_TRUE(pngWriter::write(path, 16, 16, second.data()));

	EXPECT_GT(std::filesystem::file_size(path), smallSize);

	const std::vector<uint8_t> written = readFile(path);
	EXPECT_EQ(readBigEndian(written, 16), 16u);
}

TEST_F(PngWriterTest, anImpossibleRequestIsRefused)
{
	const std::vector<uint8_t> pixels = gradient(2, 2);

	EXPECT_FALSE(pngWriter::write("", 2, 2, pixels.data()));
	EXPECT_FALSE(pngWriter::write(path, 0, 2, pixels.data()));
	EXPECT_FALSE(pngWriter::write(path, 2, 0, pixels.data()));
	EXPECT_FALSE(pngWriter::write(path, 2, 2, nullptr));
}

TEST_F(PngWriterTest, aDirectoryThatDoesNotExistIsRefused)
{
	const std::vector<uint8_t> pixels = gradient(2, 2);
	const std::string missing = (directory / "nope" / "image.png").string();

	EXPECT_FALSE(pngWriter::write(missing, 2, 2, pixels.data()));
}
