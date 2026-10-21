#include "RtosCsvExport.hpp"

#include <cstdio>

const char* RtosCsvExport::header()
{
	return "round,at_s,task,address,state,found_on,priority,base_priority,stack_used_bytes,stack_size_bytes,stack_free_percent,cpu_share_percent,"
	       "waiting_on";
}

std::string RtosCsvExport::field(const std::string& text)
{
	/* A field breaks the row if it holds the separator, the quote or a line
	   break; and a field that starts or ends with a space loses it in some
	   readers, which is the same defect with a quieter symptom. */
	bool quote = text.empty() ? false : (text.front() == ' ' || text.back() == ' ');

	for (char character : text)
	{
		if (character == ',' || character == '"' || character == '\n' || character == '\r')
		{
			quote = true;
			break;
		}
	}

	if (!quote)
		return text;

	std::string out;
	out.reserve(text.size() + 2);
	out += '"';

	for (char character : text)
	{
		if (character == '"')
			out += '"';

		out += character;
	}

	out += '"';

	return out;
}

size_t RtosCsvExport::writeRound(std::ostream& out, size_t round, double atSeconds, const std::vector<RtosTaskRow>& rows)
{
	char number[32] = {};

	for (const RtosTaskRow& row : rows)
	{
		out << round;

		out << ',';
		std::snprintf(number, sizeof(number), "%.4f", atSeconds);
		out << number;

		/* Name and address are identity rather than a reading: the session
		   knows them for a task this round could not read, and dropping them
		   would make the row unattributable. */
		out << ',' << field(row.name.empty() ? "-" : row.name);

		out << ',';
		std::snprintf(number, sizeof(number), "0x%08X", row.tcbAddress);
		out << number;

		out << ',' << (row.hasReading ? toString(row.state) : "");
		out << ',' << field(row.sourceList);

		if (row.hasPriority)
		{
			std::snprintf(number, sizeof(number), "%u", row.priority);
			out << ',' << number;
		}
		else
			out << ',';

		if (row.hasBasePriority)
		{
			std::snprintf(number, sizeof(number), "%u", row.basePriority);
			out << ',' << number;
		}
		else
			out << ',';

		/* The water mark, not the declared size: a stack that is 40% used out
		   of twice the memory is the same number as one that is 40% used out
		   of half, and only the pair tells them apart. */
		if (row.hasStackWatermark)
		{
			std::snprintf(number, sizeof(number), "%u", row.stackUsedBytes());
			out << ',' << number;

			std::snprintf(number, sizeof(number), "%u", row.stackWatermarkStackBytes);
			out << ',' << number;

			std::snprintf(number, sizeof(number), "%u", row.stackFreePercent());
			out << ',' << number;
		}
		else
			out << ",,,";

		if (row.hasCpuShare)
		{
			std::snprintf(number, sizeof(number), "%.1f", row.cpuSharePercent);
			out << ',' << number;
		}
		else
			out << ',';

		/* Empty is the answer for a task waiting on nothing, so it is written
		   the same way a missing reading is: the column says nothing rather
		   than saying zero. */
		out << ',' << field(row.waitingOn);

		out << '\n';
	}

	return rows.size();
}

RtosCsvExport::Counts RtosCsvExport::writeRecording(std::ostream& out, const RtosModel& model)
{
	Counts counts;

	if (!model.isPlayback())
		return counts;

	const RtosModel::PlaybackInfo info = model.playbackInfo();

	out << header() << '\n';

	/* Built with an empty query: the export is the recording, not what the
	   filters happened to leave on screen when the button was pressed. */
	const RtosTaskTableQuery unfiltered;

	for (size_t round = 0; round < info.rounds; round++)
	{
		const RtosModel::PlaybackRound playback = model.playbackRoundAt(round);

		if (playback.snapshot == nullptr)
		{
			counts.skipped++;
			continue;
		}

		counts.rounds++;
		counts.rows += writeRound(out, round, playback.atSeconds, RtosTableModel::taskRows(*playback.snapshot, unfiltered));
	}

	return counts;
}
