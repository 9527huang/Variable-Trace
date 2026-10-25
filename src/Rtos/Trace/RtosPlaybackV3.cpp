#include "RtosPlaybackV3.hpp"

#include "RtosTraceArchive.hpp"

#include <cstring>

namespace
{
	/* captureStartedAt, which the round encoding writes first. Reading it here
	   rather than decoding the round keeps opening a long recording cheap: the
	   interface needs this number to draw a scrollbar, not to show a task. */
	double leadingSecond(const std::vector<uint8_t>& round)
	{
		if (round.size() < sizeof(double))
			return 0.0;

		double value = 0.0;
		std::memcpy(&value, round.data(), sizeof(double));

		return value;
	}
}  // namespace

bool RtosPlaybackV3::open(const std::string& path, RtosRecording::Problem& problem)
{
	problem = RtosRecording::Problem::None;

	close();

	RtosRecordingV3 container;
	RtosRecordingV3::Contents contents;
	RtosRecordingV3::Problem problem3 = RtosRecordingV3::Problem::None;

	if (!container.open(path, contents, problem3))
	{
		/* The container's reasons are its own; the interface has one vocabulary
		   for "the file would not open", and this is it. */
		problem = problem3 == RtosRecordingV3::Problem::NotAV3File ? RtosRecording::Problem::NotARecording
		                                                          : RtosRecording::Problem::CannotOpen;

		return false;
	}

	filePath = path;
	fileBytes = contents.bytes;

	size_t blockIndex = 0;

	for (const RtosRecordingV3::Block& block : contents.blocks)
	{
		types.push_back(block.type);

		/* The three blocks a session's events live in. A block that does not
		   decode is skipped rather than failing the file, for the reason a bad
		   round is: everything before it is still a session somebody recorded. */
		if (block.type == RtosRecordingV3::BlockType::NormalizedEvents)
		{
			std::vector<RtosNormalizedEvent> batch;

			if (RtosTraceArchive::decodeEvents(block.payload, batch, block.version))
			{
				traceEvents.insert(traceEvents.end(), batch.begin(), batch.end());
				haveEvents = haveEvents || !batch.empty();
			}

			blockIndex++;
			continue;
		}

		if (block.type == RtosRecordingV3::BlockType::IdentityDictionary)
		{
			RtosIdentityStore decoded;

			if (RtosTraceArchive::decodeIdentities(block.payload, decoded))
				traceIdentities = std::move(decoded);

			blockIndex++;
			continue;
		}

		if (block.type == RtosRecordingV3::BlockType::SourceDiagnostics)
		{
			RtosTraceSourceRecord decoded;

			if (RtosTraceArchive::decodeSource(block.payload, decoded))
				sourceRecord = decoded;

			blockIndex++;
			continue;
		}

		if (block.type != RtosRecordingV3::BlockType::SnapshotRound)
		{
			blockIndex++;
			continue;
		}

		std::vector<std::vector<uint8_t>> encoded;

		if (!RtosRecordingV3::unpackRounds(block.payload, encoded))
		{
			/* A block that does not unpack is skipped rather than failing the
			   file: everything before it is still a run of rounds somebody
			   recorded, and `stoppedEarly` already said the tail is suspect. */
			blockIndex++;
			continue;
		}

		const size_t stored = blockRounds.size();
		blockRounds.push_back(std::move(encoded));

		for (size_t index = 0; index < blockRounds[stored].size(); index++)
		{
			RoundRef reference;
			reference.block = stored;
			reference.index = index;
			reference.atSeconds = leadingSecond(blockRounds[stored][index]);

			rounds.push_back(reference);
		}

		blockIndex++;
	}

	if (rounds.empty())
	{
		problem = RtosRecording::Problem::NoRounds;
		close();

		return false;
	}

	opened = true;

	return true;
}

void RtosPlaybackV3::close()
{
	blockRounds.clear();
	rounds.clear();
	types.clear();
	traceEvents.clear();
	traceEvents.shrink_to_fit();
	traceIdentities.clear();
	sourceRecord = RtosTraceSourceRecord{};
	haveEvents = false;
	filePath.clear();
	fileBytes = 0;
	opened = false;
}

double RtosPlaybackV3::atSeconds(size_t round) const
{
	return round < rounds.size() ? rounds[round].atSeconds : 0.0;
}

bool RtosPlaybackV3::decode(const RoundRef& reference, Round& out) const
{
	if (reference.block >= blockRounds.size() || reference.index >= blockRounds[reference.block].size())
		return false;

	const std::vector<uint8_t>& bytes = blockRounds[reference.block][reference.index];

	RtosRecording::Round decoded;

	/* The same decoder the v2 file uses, with the event tail included: a v3
	   round is a v2 round, stored in a block instead of after a record header. */
	if (!RtosRecording::decodeRound(bytes.data(), bytes.size(), true, decoded))
		return false;

	out.snapshot = decoded.snapshot;
	out.events = decoded.events;

	return true;
}

std::shared_ptr<RtosSnapshot> RtosPlaybackV3::roundAt(size_t round) const
{
	if (round >= rounds.size())
		return nullptr;

	Round decoded;

	if (!decode(rounds[round], decoded))
		return nullptr;

	return decoded.snapshot;
}

RtosPlaybackV3::Round RtosPlaybackV3::roundWithEvents(size_t round) const
{
	if (round >= rounds.size())
		return Round{};

	Round decoded;

	if (!decode(rounds[round], decoded))
		return Round{};

	return decoded;
}
