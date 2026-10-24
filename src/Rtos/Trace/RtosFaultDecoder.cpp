#include "RtosFaultDecoder.hpp"

#include <cstdio>

namespace
{
	/* CFSR's three halves, in the order the register lays them out. */
	constexpr uint32_t mmfsrBits = 0x000000FFu;
	constexpr uint32_t bfsrBits = 0x0000FF00u;
	constexpr uint32_t ufsrBits = 0xFFFF0000u;

	bool bit(uint32_t value, unsigned index)
	{
		return (value & (1u << index)) != 0u;
	}

	RtosFaultLine line(const char* code, std::string meaning)
	{
		RtosFaultLine result;

		result.code = code;
		result.meaning = std::move(meaning);

		return result;
	}

	std::string addressText(uint32_t address)
	{
		char text[32];

		std::snprintf(text, sizeof(text), "0x%08X", address);

		return text;
	}
}  // namespace

std::string RtosFaultReport::summary() const
{
	if (lines.empty())
		return "the status register was empty, so only the PC is evidence";

	return lines.front().meaning;
}

RtosFaultReport decodeFault(uint32_t cfsr, uint32_t pc, uint32_t lr)
{
	RtosFaultReport report;

	report.cfsr = cfsr;
	report.pc = pc;
	report.lr = lr;

	const uint32_t mmfsr = cfsr & mmfsrBits;
	const uint32_t bfsr = (cfsr & bfsrBits) >> 8;
	const uint32_t ufsr = (cfsr & ufsrBits) >> 16;

	/* Memory management. On a part without an MPU these cannot be set, and if
	   one of them is set the part has one and nobody configured it. */
	if (bit(mmfsr, 0))
		report.lines.push_back(line("IACCVIOL", "an instruction was fetched from an address the MPU forbids"));

	if (bit(mmfsr, 1))
	{
		report.lines.push_back(line("DACCVIOL", "a data access hit an address the MPU forbids"));
	}

	if (bit(mmfsr, 3))
		report.lines.push_back(line("MUNSTKERR", "the exception return could not pop the stack - the stack pointer was already lost"));

	if (bit(mmfsr, 4))
		report.lines.push_back(line("MSTKERR", "the exception could not push its frame - the stack pointer was already lost"));

	/* Bus faults. */
	if (bit(bfsr, 0))
		report.lines.push_back(line("IBUSERR", "a prefetch went to an address that does not answer"));

	if (bit(bfsr, 1))
	{
		report.lines.push_back(line("PRECISERR", "an access hit an address that does not answer, and the fault is precise"));
	}

	if (bit(bfsr, 2))
	{
		/* The one everybody has to be told about: the store buffer reported it
		   late, so the PC no longer points at the instruction that caused it. */
		report.lines.push_back(line("IMPRECISERR", "an access hit an address that does not answer, reported late - the PC points past the cause, not at it"));
	}

	if (bit(bfsr, 3))
		report.lines.push_back(line("UNSTKERR", "the exception return could not pop the stack"));

	if (bit(bfsr, 4))
		report.lines.push_back(line("STKERR", "the exception could not push its frame"));

	/* Usage faults - the ones a program causes rather than a bus. */
	if (bit(ufsr, 0))
		report.lines.push_back(line("UNDEFINSTR", "the core was asked to execute something that is not an instruction"));

	if (bit(ufsr, 1))
	{
		/* The single most common crash in embedded C, and worth its own
		   sentence: an odd PC means a branch to a Thumb address, and a null
		   function pointer is exactly that. */
		RtosFaultLine entry = line("INVSTATE", (pc & 1u) == 0u ? "a branch went to an address that is not Thumb code - a null or uninitialised function pointer looks like this"
		                                                      : "the core was left in an invalid execution state");

		report.lines.push_back(entry);
	}

	if (bit(ufsr, 2))
		report.lines.push_back(line("INVPC", "the exception return loaded a PC that is not a legal address"));

	if (bit(ufsr, 3))
		report.lines.push_back(line("NOCP", "the code used a coprocessor, and this part does not have it"));

	if (bit(ufsr, 8))
		report.lines.push_back(line("UNALIGNED", "a load or store was not aligned for its width"));

	if (bit(ufsr, 9))
		report.lines.push_back(line("DIVBYZERO", "an integer division by zero"));

	/* An address, when one was captured. The register is only meaningful when
	   its valid bit is set - reading it otherwise yields whatever was there
	   before, which looks like an address and is not one. */
	if (bit(bfsr, 7) || bit(mmfsr, 7))
	{
		/* The firmware sends only the three values it read, so the address is
		   not among them yet; say so rather than inventing one. */
		report.lines.push_back(line("BFARVALID", "the faulting address was captured by the target, but this build does not carry it in the event"));
	}

	if (report.lines.empty() && cfsr != 0)
		report.lines.push_back(line("CFSR", "the status register has a bit set that this build does not name: " + addressText(cfsr)));

	return report;
}
