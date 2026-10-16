#ifndef _GDBRTOSLAYOUT_HPP
#define _GDBRTOSLAYOUT_HPP

#include <string>
#include <vector>

#include "RtosLayout.hpp"
#include "RtosProfileBase.hpp"
#include "spdlog/spdlog.h"

/*
 * The structure layout as the debug information has it.
 *
 * Every offset in a kernel's control block is a property of the build, not of
 * the kernel version. Turning on the list integrity checking options puts bytes
 * at the head of `List_t` and `ListItem_t` and moves everything behind them; a
 * table copied out of one build then reads the wrong bytes on the next, and
 * what it reads looks like data. That is the failure this reader exists to
 * remove: the offsets come from the same symbol file the addresses do.
 *
 * The numbers here are all `Measured`, so a table built by this reader says so
 * and the interface stops warning about written down values.
 */
class GdbRtosLayout
{
   public:
	/*
	 * Reads the layout of every structure the profile names.
	 *
	 * The symbol file is opened once and every structure is asked about over
	 * that one session. A structure the debug information does not describe is
	 * simply absent from the answer, and the profile falls back for its fields
	 * alone.
	 */
	static RtosLayoutTable read(const std::string& gdbCommand, const std::string& elfPath, const RtosProfileBase& profile, spdlog::logger* logger);

	/*
	 * Parses one `ptype /o` dump and files it under the name it was asked for.
	 *
	 * The name matters: gdb prints the structure's own tag (`xLIST_ITEM`) and
	 * the profile calls it by the typedef it knows (`ListItem_t`), so a parser
	 * that used what it found in the text would file every field under a name
	 * nothing asks for.
	 *
	 * Kept apart from the run above so that it can be tested against fixed
	 * text. What a toolchain prints has changed between releases and will
	 * again, and a parser checked only against today's output is a parser that
	 * breaks quietly on the next upgrade.
	 */
	static bool parseStructDump(const std::string& dump, const std::string& structureName, RtosLayoutTable& table);

	/* The structure names a profile asks about, in the order it names them and
	   without repeats. */
	static std::vector<std::string> structuresOf(const RtosProfileBase& profile);
};

#endif
