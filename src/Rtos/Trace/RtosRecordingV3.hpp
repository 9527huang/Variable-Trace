#ifndef _RTOSRECORDINGV3_HPP
#define _RTOSRECORDINGV3_HPP

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

/*
 * The v3 recording: a container of committed blocks.
 *
 * The v2 file was one record after another with a length and a magic. It worked
 * because there was only one kind of thing in it. There are now at least eight -
 * raw records, normalized events, snapshot rounds, a dictionary, source
 * diagnostics, the rule set, an index - and a format that can only hold one of
 * them would need a new version for each.
 *
 * So each block says what it is, and - this is the part that makes crash
 * recovery possible without a repair tool - a block that was not finished does
 * not look finished. The block header carries a CRC over the payload and a
 * commit word written last; a reader that finds either missing stops there and
 * keeps everything before it. There is no repair pass because a repaired record
 * is a record the writer never promised.
 *
 * Old files are not converted. `RtosRecording` still reads v1 and v2; this class
 * only names them as "not v3" so that the caller can pick the right reader
 * rather than getting a confusing error from the wrong one.
 */
class RtosRecordingV3
{
   public:
	enum class BlockType : uint16_t
	{
		SessionManifest = 1,
		RawTrace = 2,
		NormalizedEvents = 3,
		SnapshotRound = 4,
		IdentityDictionary = 5,
		SourceDiagnostics = 6,
		RuleConfiguration = 7,
		IndexCheckpoint = 8,
	};

	enum class Problem
	{
		None,
		CannotCreate,
		CannotOpen,
		NotAV3File,
		NotWritable,
		CannotWrite,
		NoBlocks,
	};

	static const char* describe(Problem problem);

	/* The eight bytes that say which file this is: 'V','T','R','C' and the
	   version, then the header length. A v1/v2 file begins with 'VTRECORD' and
	   is left to the old reader. */
	static bool looksLikeV3(const std::string& path);

	/* Every index checkpoint is written after this many data blocks. */
	static constexpr uint32_t checkpointEvery = 64;

	/* ---- writing ---- */

	bool create(const std::string& path, uint64_t createdUnix, Problem& problem);
	/* Appends a data block. Every `checkpointEvery` data blocks an index
	   checkpoint is written after it, so that a reader can jump to a time
	   window without scanning the file. */
	bool append(BlockType type, const std::vector<uint8_t>& payload, uint16_t version, Problem& problem);
	void close();

	bool isOpen() const
	{
		return file != nullptr;
	}

	uint32_t blocksWritten() const
	{
		return written;
	}

	/* Where the data blocks went, in order, for the index checkpoint. 24 bytes
	   each on disk; kept in memory until the file is closed, which for a ten
	   minute recording is a few hundred kilobytes. */
	struct IndexEntry
	{
		uint32_t sequence = 0;
		uint64_t payloadOffset = 0;
		uint32_t payloadBytes = 0;
		uint32_t firstTicks = 0;
		uint32_t lastTicks = 0;
	};

	const std::vector<IndexEntry>& index() const
	{
		return entries;
	}

	/* ---- reading ---- */

	struct Block
	{
		BlockType type = BlockType::SessionManifest;
		uint16_t version = 0;
		uint32_t sequence = 0;
		uint64_t payloadOffset = 0;
		std::vector<uint8_t> payload;

		/* Plain bytes for a caller that wants to hash or compare them without
		   copying the payload again. */
		std::vector<uint8_t> raw;
	};

	struct Contents
	{
		std::vector<Block> blocks;

		/* True when the file ended in something that was not a finished block:
		   a crash, a truncated copy, a bad CRC. `blocks` still holds every
		   block before that point. */
		bool stoppedEarly = false;

		/* Which check failed, for the log. */
		std::string stoppedBecause;

		uint64_t bytes = 0;
	};

	/* Reads the whole file. A file with no committed block at all is refused:
	   the container is not the question, the data is. */
	bool open(const std::string& path, Contents& contents, Problem& problem);

	/*
	 * Rounds inside a block.
	 *
	 * The payload is: u32 count, then for each round a u32 length and the
	 * round's bytes in the encoding `RtosRecording::encodeRound` produces. The
	 * length is repeated per round rather than implied, so that a block written
	 * by a future version that adds a field still decodes as far as it goes -
	 * the alternative is one unknown byte shifting every round after it.
	 */
	static std::vector<uint8_t> packRounds(const std::vector<std::vector<uint8_t>>& rounds);
	static bool unpackRounds(const std::vector<uint8_t>& payload, std::vector<std::vector<uint8_t>>& rounds);

	/* ---- helpers shared with the tests ---- */

	/* CRC-32/MPEG-2: polynomial 0x04C11DB7, init 0xFFFFFFFF, no reflection, no
	   final xor. The check value for "123456789" is 0x0376E6E7 and the tests
	   hold it to that, because a CRC that only agrees with itself proves
	   nothing. */
	static uint32_t crc32(const uint8_t* bytes, size_t size);

   private:
	bool writeBlock(const uint8_t* header, const std::vector<uint8_t>& payload);
	bool writeAny(BlockType type, const std::vector<uint8_t>& payload, uint16_t version, Problem& problem);

	std::vector<IndexEntry> entries;

	std::FILE* file = nullptr;
	uint32_t written = 0;
	uint64_t offset = 0;
	bool failed = false;
};

#endif
