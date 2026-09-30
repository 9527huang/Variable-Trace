#ifndef _ELFTARGET_HPP
#define _ELFTARGET_HPP

#include <cstdint>
#include <string>

/*
 * Which processor a symbol file was built for.
 *
 * The application only cares about one answer here. A TI C2000 counts memory
 * in 16 bit words, so every address a symbol file for it carries is a word
 * address, and the debug probe reads bytes. Feeding the probe a word address
 * reads the wrong memory, silently: the address is valid, the value is not.
 * The variable table therefore has to know, before it parses, whether the file
 * belongs to a target that counts in words.
 *
 * The answer is read from the file header alone, without loading the symbols:
 *
 *   - an ELF header names the machine in `e_machine`, and the C2000 family has
 *     its own value there;
 *   - an older TI toolchain writes COFF instead of ELF, and puts the same
 *     information in the first two bytes of the file header.
 *
 * The COFF branch recognises the C2000 family magic only. Files that carry the
 * generic COFF version word in that field cannot be told apart by family from
 * the header, so they come back as Other and are left to the user to switch by
 * hand.
 */
enum class ElfTarget : uint8_t
{
	/* The file could not be read, or it is too short to have a header. */
	Unknown = 0,
	/* A target that counts memory in 16 bit words. */
	C2000 = 1,
	/* A readable object file that is not a C2000 one. */
	Other = 2,
};

/* Reads the file header of `path`. Never throws: a missing or unreadable file
   is Unknown, which the caller treats as "no reason to suggest anything". */
ElfTarget detectElfTarget(const std::string& path);

std::string describeElfTarget(ElfTarget target);

#endif
