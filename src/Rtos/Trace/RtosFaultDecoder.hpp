#ifndef _RTOSFAULTDECODER_HPP
#define _RTOSFAULTDECODER_HPP

#include <cstdint>
#include <string>
#include <vector>

/*
 * CFSR, said in words.
 *
 * The target sends three numbers when it faults - the configurable fault status
 * register, the stacked PC and the stacked LR - and those three numbers are the
 * whole of what a crash arrives with. `0x00008200` is not an answer to anything;
 * "a write to an address that does not exist, at 0x08001A2C" is.
 *
 * The decoding is here rather than in the panel because it is the part that can
 * be wrong: bit 1 of a register read from a device that is no longer running is
 * worth a test, and a panel is not.
 *
 * Two combinations deserve their own sentence, because they are the two that
 * happen most and neither is obvious from the individual bits:
 *
 *   INVSTATE with a PC whose bit 0 is clear - a branch to something that is not
 *   a function at all, which in practice is a null or uninitialised handler.
 *
 *   PRECISERR without BFARVALID - the fault is precise, so the address is
 *   knowable, but the register was not captured; the PC still says where.
 */

struct RtosFaultLine
{
	/* The bit's name as the architecture spells it, kept untranslated: it is a
	   term of art, and somebody reading a crash report will search for it. */
	std::string code;

	/* What it means, in a sentence. */
	std::string meaning;

	/* CFSR carries no read/write direction - bit 7 of each half is the
	   "address register is valid" flag, not a direction. A direction would have
	   to come from the fault address register beside it, which this firmware
	   does not send yet. */
	bool hasAddress = false;
	uint32_t address = 0;
};

struct RtosFaultReport
{
	uint32_t cfsr = 0;
	uint32_t pc = 0;
	uint32_t lr = 0;

	/* Zero when CFSR says nothing at all, which is itself worth saying: the
	   fault happened but the status register was cleared, so only the PC is
	   evidence. */
	std::vector<RtosFaultLine> lines;

	bool empty() const
	{
		return lines.empty();
	}

	/* One line for a table column: the first finding, or the fallback. */
	std::string summary() const;
};

/* `cfsr` is the register, `pc` and `lr` the stacked values the handler read. */
RtosFaultReport decodeFault(uint32_t cfsr, uint32_t pc, uint32_t lr);

#endif
