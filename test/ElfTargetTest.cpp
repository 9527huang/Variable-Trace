#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ElfTarget.hpp"

/*
 * Reading the target out of a file header.
 *
 * The answer decides one thing: whether the user is offered the parser that
 * counts memory the way the target does. Choosing the wrong parser here does
 * not fail loudly - it produces addresses that are half of what they should be,
 * and a read that returns a valid number from the wrong place - so the check
 * itself is worth pinning down.
 */

namespace
{
	class ElfTargetTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			directory = std::filesystem::temp_directory_path() / "variable-trace-elf-target-test";
			std::filesystem::create_directories(directory);
		}

		void TearDown() override
		{
			std::error_code errorCode;
			std::filesystem::remove_all(directory, errorCode);
		}

		std::string write(const std::string& name, const std::vector<uint8_t>& bytes)
		{
			const std::filesystem::path path = directory / name;

			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			file.close();

			return path.string();
		}

		/* An ELF header, of which the four ident bytes and the two machine
		   bytes are the whole of what is read. The rest is zero padding, so a
		   field that is read from the wrong offset shows up as a failure. */
		static std::vector<uint8_t> elfHeader(uint16_t machine, uint8_t elfClass = 1, uint8_t data = 1)
		{
			std::vector<uint8_t> header(20, 0);

			header[0] = 0x7F;
			header[1] = 'E';
			header[2] = 'L';
			header[3] = 'F';
			header[4] = elfClass;
			header[5] = data;
			header[6] = 1;
			header[16] = 0x02;
			header[17] = 0x00;

			if (data == 1)
			{
				header[18] = static_cast<uint8_t>(machine & 0xFF);
				header[19] = static_cast<uint8_t>((machine >> 8) & 0xFF);
			}
			else
			{
				header[18] = static_cast<uint8_t>((machine >> 8) & 0xFF);
				header[19] = static_cast<uint8_t>(machine & 0xFF);
			}

			return header;
		}

		std::filesystem::path directory;
	};

	constexpr uint16_t machineArm = 40;
	constexpr uint16_t machineTiC2000 = 141;
	constexpr uint16_t machineRiscV = 243;
	constexpr uint16_t machineTiC6000 = 140;
}  // namespace

TEST_F(ElfTargetTest, aC2000ElfIsRecognised)
{
	const std::string path = write("c2000.elf", elfHeader(machineTiC2000));

	EXPECT_EQ(detectElfTarget(path), ElfTarget::C2000);
}

TEST_F(ElfTargetTest, theNeighbouringTiMachinesAreNotC2000)
{
	/* C6000 sits one below C2000 in the machine list and is a different family,
	   with a different word size. */
	const std::string c6000 = write("c6000.elf", elfHeader(machineTiC6000));

	EXPECT_EQ(detectElfTarget(c6000), ElfTarget::Other);
}

TEST_F(ElfTargetTest, aBigEndianC2000ElfIsRecognised)
{
	const std::string path = write("c2000-be.elf", elfHeader(machineTiC2000, 1, 2));

	EXPECT_EQ(detectElfTarget(path), ElfTarget::C2000);
}

TEST_F(ElfTargetTest, a64BitC2000ElfIsRecognised)
{
	const std::string path = write("c2000-64.elf", elfHeader(machineTiC2000, 2, 1));

	EXPECT_EQ(detectElfTarget(path), ElfTarget::C2000);
}

TEST_F(ElfTargetTest, anArmElfIsNotC2000)
{
	const std::string path = write("arm.elf", elfHeader(machineArm));

	EXPECT_EQ(detectElfTarget(path), ElfTarget::Other);
}

TEST_F(ElfTargetTest, aRiscVElfIsNotC2000)
{
	const std::string path = write("riscv.elf", elfHeader(machineRiscV, 2, 1));

	EXPECT_EQ(detectElfTarget(path), ElfTarget::Other);
}

TEST_F(ElfTargetTest, anInvalidElfClassIsNotTreatedAsAFile)
{
	std::vector<uint8_t> bytes = elfHeader(machineTiC2000);
	bytes[4] = 9;

	const std::string path = write("bad-class.elf", bytes);

	EXPECT_EQ(detectElfTarget(path), ElfTarget::Unknown);
}

TEST_F(ElfTargetTest, anInvalidByteOrderIsNotTreatedAsAFile)
{
	std::vector<uint8_t> bytes = elfHeader(machineTiC2000);
	bytes[5] = 7;

	const std::string path = write("bad-data.elf", bytes);

	EXPECT_EQ(detectElfTarget(path), ElfTarget::Unknown);
}

TEST_F(ElfTargetTest, aTruncatedElfHeaderIsNotTreatedAsAFile)
{
	/* Long enough to carry the ident bytes, too short to carry a machine. */
	std::vector<uint8_t> bytes = elfHeader(machineTiC2000);
	bytes.resize(18);

	const std::string path = write("truncated.elf", bytes);

	EXPECT_EQ(detectElfTarget(path), ElfTarget::Unknown);
}

TEST_F(ElfTargetTest, anOlderToolchainsC2000CoffIsRecognised)
{
	/* The COFF format carries the device family in the first two bytes, written
	   in the target's own byte order. TMS320C2800 is the C2000 core. */
	const std::string path = write("c2000.out", {0x9D, 0x00, 0x20, 0x00});

	EXPECT_EQ(detectElfTarget(path), ElfTarget::C2000);
}

TEST_F(ElfTargetTest, anotherTiCoffFamilyIsNotC2000)
{
	const std::string path = write("c6000.out", {0x99, 0x00, 0x20, 0x00});

	EXPECT_EQ(detectElfTarget(path), ElfTarget::Other);
}

TEST_F(ElfTargetTest, aCoffFileWithTheGenericVersionWordIsNotC2000)
{
	/* The two COFF versions put a version stamp where other TI toolchains put
	   the family. The family cannot be read from such a header, so nothing is
	   claimed about it; the parser is chosen by hand in that case. */
	const std::string path = write("generic.out", {0xC2, 0x00, 0x20, 0x00});

	EXPECT_EQ(detectElfTarget(path), ElfTarget::Other);
}

TEST_F(ElfTargetTest, aFileThatIsNotAnObjectFileIsUnknown)
{
	const std::string path = write("notes.txt", {'h', 'e', 'l', 'l', 'o', '\n'});

	EXPECT_EQ(detectElfTarget(path), ElfTarget::Unknown);
}

TEST_F(ElfTargetTest, aMissingFileIsUnknown)
{
	EXPECT_EQ(detectElfTarget((directory / "nothing-here.elf").string()), ElfTarget::Unknown);
}

TEST_F(ElfTargetTest, anEmptyPathIsUnknown)
{
	EXPECT_EQ(detectElfTarget(""), ElfTarget::Unknown);
}

TEST_F(ElfTargetTest, theNamesAreStable)
{
	/* The names are for a log line, but a change to one of them is still a
	   change of what the user is told. */
	EXPECT_EQ(describeElfTarget(ElfTarget::C2000), "TI C2000");
	EXPECT_EQ(describeElfTarget(ElfTarget::Other), "other");
	EXPECT_EQ(describeElfTarget(ElfTarget::Unknown), "unknown");
}
