#include "ElfTarget.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	/* The first four bytes of every ELF file, and the two ident bytes that say
	   how wide the fields are and which byte order they use. */
	constexpr char elfMagic[4] = {0x7F, 'E', 'L', 'F'};
	constexpr size_t elfClassIndex = 4;
	constexpr size_t elfDataIndex = 5;
	constexpr size_t elfMachineOffset = 18;
	constexpr size_t elfHeaderProbeSize = 20;

	constexpr uint8_t elfClass32 = 1;
	constexpr uint8_t elfClass64 = 2;
	constexpr uint8_t elfDataLittleEndian = 1;
	constexpr uint8_t elfDataBigEndian = 2;

	/* e_machine for the TI C2000 family, from the System V gABI machine list
	   (TI C6000 = 140, C2000 = 141, C5500 = 142). */
	constexpr uint16_t machineTiC2000 = 141;

	/* TI COFF object file header, where the first two bytes name the device
	   family. The C2000 core is TMS320C2800, whose magic is the one that
	   matters here; the rest of the family's table is kept so that a TI object
	   file for some other processor is recognised as one, rather than being
	   reported as a file that could not be read. */
	constexpr uint16_t coffMagicTiC2800 = 0x009D;
	constexpr uint16_t coffMagicTiTms470 = 0x0097;
	constexpr uint16_t coffMagicTiC5400 = 0x0098;
	constexpr uint16_t coffMagicTiC6000 = 0x0099;
	constexpr uint16_t coffMagicTiC5500 = 0x009C;
	constexpr uint16_t coffMagicTiMsp430 = 0x00A0;
	constexpr uint16_t coffMagicTiC5500Plus = 0x00A1;
	constexpr uint16_t coffVersionWord1 = 0x00C1;
	constexpr uint16_t coffVersionWord2 = 0x00C2;
	constexpr size_t coffMagicSize = 2;

	std::vector<uint8_t> readHead(const std::string& path, size_t count)
	{
		std::vector<uint8_t> buffer(count, 0);

		std::FILE* file = std::fopen(path.c_str(), "rb");

		if (file == nullptr)
			return {};

		const size_t read = std::fread(buffer.data(), 1, count, file);
		std::fclose(file);

		buffer.resize(read);

		return buffer;
	}

	uint16_t read16(const std::vector<uint8_t>& data, size_t offset, bool littleEndian)
	{
		if (offset + 2 > data.size())
			return 0;

		if (littleEndian)
			return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));

		return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
	}
}  // namespace

ElfTarget detectElfTarget(const std::string& path)
{
	const std::vector<uint8_t> head = readHead(path, elfHeaderProbeSize);

	if (head.size() < coffMagicSize)
		return ElfTarget::Unknown;

	const bool isElf = head.size() >= 6 &&
					   std::memcmp(head.data(), elfMagic, sizeof(elfMagic)) == 0;

	if (isElf)
	{
		if (head[elfClassIndex] != elfClass32 && head[elfClassIndex] != elfClass64)
			return ElfTarget::Unknown;

		if (head[elfDataIndex] != elfDataLittleEndian && head[elfDataIndex] != elfDataBigEndian)
			return ElfTarget::Unknown;

		const bool littleEndian = head[elfDataIndex] == elfDataLittleEndian;

		if (head.size() < elfMachineOffset + 2)
			return ElfTarget::Unknown;

		const uint16_t machine = read16(head, elfMachineOffset, littleEndian);

		return machine == machineTiC2000 ? ElfTarget::C2000 : ElfTarget::Other;
	}

	/* Not an ELF file. The only other container this knows about is the TI COFF
	   format, whose family magic sits at the very start of the header and is
	   written in the target's own byte order. A COFF file that carries the
	   generic version word there instead cannot be told apart by family, so it
	   is reported as some other target and the user picks the parser by hand. */
	const uint16_t firstWord = read16(head, 0, true);

	if (firstWord == coffMagicTiC2800)
		return ElfTarget::C2000;

	switch (firstWord)
	{
		case coffMagicTiTms470:
		case coffMagicTiC5400:
		case coffMagicTiC6000:
		case coffMagicTiC5500:
		case coffMagicTiMsp430:
		case coffMagicTiC5500Plus:
		case coffVersionWord1:
		case coffVersionWord2:
			return ElfTarget::Other;

		default:
			return ElfTarget::Unknown;
	}
}

std::string describeElfTarget(ElfTarget target)
{
	switch (target)
	{
		case ElfTarget::C2000:
			return "TI C2000";

		case ElfTarget::Other:
			return "other";

		default:
			return "unknown";
	}
}
