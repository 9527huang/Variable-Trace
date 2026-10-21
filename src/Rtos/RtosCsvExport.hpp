#ifndef _RTOSCSVEXPORT_HPP
#define _RTOSCSVEXPORT_HPP

#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

#include "RtosModel.hpp"
#include "RtosTableModel.hpp"

/*
 * The recording, as a table something else can read.
 *
 * A recorded session is a file this program can open and no other program can.
 * The questions people bring to it - does this stack creep up over an hour,
 * which task owns the time on this build, how often does that one block - are
 * questions a spreadsheet answers, and answering them here would mean building
 * a second, smaller spreadsheet.
 *
 * So this writes the same rows the table shows, one line per task per round,
 * and nothing else. The columns are the table's columns, spelled for a machine
 * (English, snake case); the empty cell is the same statement the table's dash
 * is, and it is written as an empty field rather than as a zero - a zero is a
 * reading, and "this round did not measure it" is not.
 *
 * The round number and its time sit on every line on purpose. A CSV is read by
 * filtering and grouping, and both want a key on the row rather than a section
 * break the reader has to notice.
 */
class RtosCsvExport
{
   public:
	/* The header line, without the newline. Written once, on the first round. */
	static const char* header();

	/*
	 * One round's rows, oldest first as the table holds them.
	 *
	 * Returns how many tasks were written. The rows arrive already built: which
	 * tasks a round has, and what counts as a reading, are decided by
	 * `RtosTableModel`, and a second opinion here would be a second rule.
	 */
	static size_t writeRound(std::ostream& out, size_t round, double atSeconds, const std::vector<RtosTaskRow>& rows);

	/* What an export wrote, so the interface can say it without guessing. */
	struct Counts
	{
		size_t rows = 0;
		size_t rounds = 0;

		/* Rounds the file could not give back. Counted rather than fatal: the
		   rounds after a bad one are still good, and half an export reported
		   as a failure would be thrown away for nothing. */
		size_t skipped = 0;
	};

	/*
	 * The whole open recording, header first.
	 *
	 * The loop lives here rather than in the panel so that it can be held to a
	 * test: the panel is not in the test binary, and a walk over a file with a
	 * truncated round in the middle is exactly the kind of thing that only
	 * breaks on somebody else's recording.
	 */
	static Counts writeRecording(std::ostream& out, const RtosModel& model);

	/* One field, quoted when it would otherwise break the row. */
	static std::string field(const std::string& text);
};

#endif
