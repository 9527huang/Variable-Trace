#include "RtosRecordingV3.hpp"

#include <cstring>

namespace
{
	constexpr size_t fileHeaderBytes = 32;
	constexpr size_t blockHeaderBytes = 24;

	const char fileMagic[4] = {'V', 'T', 'R', 'C'};
	const char blockMagic[4] = {'B', 'L', 'C', 'K'};

	/* The commit word: 'COMT'. Only a block whose header ends with this is
	   finished; anything else is the tail of a write that stopped. */
	constexpr uint32_t commitWord = 0x544D4F43u;

	constexpr uint16_t formatVersion = 3;

	uint32_t table[256];
	bool tableReady = false;

	void buildTable()
	{
		for (uint32_t index = 0; index < 256; index++)
		{
			uint32_t value = index << 24;

			for (int bit = 0; bit < 8; bit++)
				value = (value & 0x80000000u) != 0 ? (value << 1) ^ 0x04C11DB7u : (value << 1);

			table[index] = value;
		}

		tableReady = true;
	}

	void putU16(uint8_t* at, uint16_t value)
	{
		at[0] = static_cast<uint8_t>(value & 0xFF);
		at[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
	}

	void putU32(uint8_t* at, uint32_t value)
	{
		at[0] = static_cast<uint8_t>(value & 0xFF);
		at[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
		at[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
		at[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
	}

	void putU64(uint8_t* at, uint64_t value)
	{
		putU32(at, static_cast<uint32_t>(value & 0xFFFFFFFFu));
		putU32(at + 4, static_cast<uint32_t>(value >> 32));
	}

	uint16_t readU16(const uint8_t* at)
	{
		return static_cast<uint16_t>(at[0] | (at[1] << 8));
	}

	uint32_t readU32(const uint8_t* at)
	{
		return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) | (static_cast<uint32_t>(at[2]) << 16) |
		       (static_cast<uint32_t>(at[3]) << 24);
	}
}  // namespace

const char* RtosRecordingV3::describe(Problem problem)
{
	switch (problem)
	{
		case Problem::CannotCreate:
			return "the recording could not be created";

		case Problem::CannotOpen:
			return "the recording could not be opened";

		case Problem::NotAV3File:
			return "this is not a version 3 recording";

		case Problem::NotWritable:
			return "the recording is not open for writing";

		case Problem::CannotWrite:
			return "the recording stopped taking bytes";

		case Problem::NoBlocks:
			return "this recording holds no finished block";

		case Problem::None:
		default:
			return "";
	}
}

uint32_t RtosRecordingV3::crc32(const uint8_t* bytes, size_t size)
{
	if (!tableReady)
		buildTable();

	uint32_t crc = 0xFFFFFFFFu;

	for (size_t index = 0; index < size; index++)
		crc = (crc << 8) ^ table[((crc >> 24) ^ bytes[index]) & 0xFF];

	return crc;
}

bool RtosRecordingV3::looksLikeV3(const std::string& path)
{
	std::FILE* probe = std::fopen(path.c_str(), "rb");

	if (probe == nullptr)
		return false;

	uint8_t head[8] = {};

	const bool read = std::fread(head, 1, sizeof(head), probe) == sizeof(head);

	std::fclose(probe);

	return read && std::memcmp(head, fileMagic, 4) == 0 && readU16(head + 4) == formatVersion;
}

bool RtosRecordingV3::create(const std::string& path, uint64_t createdUnix, Problem& problem)
{
	problem = Problem::None;

	close();

	file = std::fopen(path.c_str(), "wb");

	if (file == nullptr)
	{
		problem = Problem::CannotCreate;

		return false;
	}

	std::setvbuf(file, nullptr, _IOFBF, 64 * 1024);

	uint8_t header[fileHeaderBytes] = {};
	std::memcpy(header, fileMagic, 4);
	putU16(header + 4, formatVersion);
	putU16(header + 6, static_cast<uint16_t>(fileHeaderBytes));
	putU32(header + 8, 0);
	putU64(header + 16, createdUnix);

	if (std::fwrite(header, 1, sizeof(header), file) != sizeof(header))
	{
		std::fclose(file);
		file = nullptr;
		problem = Problem::CannotCreate;

		return false;
	}

	written = 0;
	offset = sizeof(header);
	failed = false;
	entries.clear();

	return true;
}

bool RtosRecordingV3::append(BlockType type, const std::vector<uint8_t>& payload, uint16_t version, Problem& problem)
{
	problem = Problem::None;

	if (file == nullptr)
	{
		problem = Problem::NotWritable;

		return false;
	}

	if (!writeAny(type, payload, version, problem))
		return false;

	/* A checkpoint every so many data blocks, written after the block it
	   indexes so that it always describes something already committed. */
	if (type != BlockType::IndexCheckpoint && written % checkpointEvery == 0)
	{
		std::vector<uint8_t> checkpoints;
		checkpoints.reserve(entries.size() * 24);

		for (const IndexEntry& entry : entries)
		{
			uint8_t item[24] = {};

			putU32(item + 0, entry.sequence);
			putU64(item + 4, entry.payloadOffset);
			putU32(item + 12, entry.payloadBytes);
			putU32(item + 16, entry.firstTicks);
			putU32(item + 20, entry.lastTicks);

			checkpoints.insert(checkpoints.end(), item, item + sizeof(item));
		}

		if (!writeAny(BlockType::IndexCheckpoint, checkpoints, 1, problem))
			return false;
	}

	return true;
}

bool RtosRecordingV3::writeAny(BlockType type, const std::vector<uint8_t>& payload, uint16_t version, Problem& problem)
{
	uint8_t header[blockHeaderBytes] = {};
	std::memcpy(header, blockMagic, 4);
	putU16(header + 4, static_cast<uint16_t>(type));
	putU16(header + 6, version);
	putU32(header + 8, static_cast<uint32_t>(payload.size()));
	putU32(header + 12, written + 1);
	putU32(header + 16, crc32(payload.data(), payload.size()));
	/* The commit word is written last, after the payload is on disk. */

	const uint64_t payloadOffset = offset + blockHeaderBytes;

	if (!writeBlock(header, payload))
	{
		problem = Problem::CannotWrite;

		return false;
	}

	written++;
	offset += blockHeaderBytes + payload.size();

	/* A checkpoint is about where the data is, so it is not itself indexed. */
	if (type != BlockType::IndexCheckpoint)
	{
		IndexEntry entry;
		entry.sequence = written;
		entry.payloadOffset = payloadOffset;
		entry.payloadBytes = static_cast<uint32_t>(payload.size());
		entry.firstTicks = 0;
		entry.lastTicks = 0;

		entries.push_back(entry);
	}

	return true;
}

bool RtosRecordingV3::writeBlock(const uint8_t* header, const std::vector<uint8_t>& payload)
{
	/* Header first, with the commit word still zero: a reader that arrives in
	   the middle of this sees a block that is not finished, which is exactly
	   what it is. */
	if (std::fwrite(header, 1, blockHeaderBytes, file) != blockHeaderBytes)
	{
		failed = true;

		return false;
	}

	if (!payload.empty() && std::fwrite(payload.data(), 1, payload.size(), file) != payload.size())
	{
		failed = true;

		return false;
	}

	/* Then the commit word, in place, and only after the bytes it vouches for
	   are written. */
	const long commitAt = static_cast<long>(offset + 20);

	if (std::fseek(file, commitAt, SEEK_SET) != 0)
	{
		failed = true;

		return false;
	}

	uint8_t commit[4] = {};
	putU32(commit, commitWord);

	if (std::fwrite(commit, 1, sizeof(commit), file) != sizeof(commit))
	{
		failed = true;

		return false;
	}

	if (std::fseek(file, 0, SEEK_END) != 0)
	{
		failed = true;

		return false;
	}

	/* Flushed here rather than at close: a crash one block later must still
	   leave this one readable. */
	if (std::fflush(file) != 0)
	{
		failed = true;

		return false;
	}

	return true;
}

void RtosRecordingV3::close()
{
	if (file != nullptr)
	{
		std::fclose(file);
		file = nullptr;
	}
}

bool RtosRecordingV3::open(const std::string& path, Contents& contents, Problem& problem)
{
	problem = Problem::None;
	contents = Contents{};

	std::FILE* source = std::fopen(path.c_str(), "rb");

	if (source == nullptr)
	{
		problem = Problem::CannotOpen;

		return false;
	}

	uint8_t header[fileHeaderBytes] = {};

	if (std::fread(header, 1, sizeof(header), source) != sizeof(header) || std::memcmp(header, fileMagic, 4) != 0)
	{
		std::fclose(source);
		problem = Problem::NotAV3File;

		return false;
	}

	if (readU16(header + 4) != formatVersion)
	{
		std::fclose(source);
		problem = Problem::NotAV3File;

		return false;
	}

	std::fseek(source, 0, SEEK_END);
	contents.bytes = static_cast<uint64_t>(std::ftell(source));
	std::fseek(source, static_cast<long>(fileHeaderBytes), SEEK_SET);

	uint32_t expectedSequence = 1;

	while (true)
	{
		const long at = std::ftell(source);

		if (at < 0 || static_cast<uint64_t>(at) + blockHeaderBytes > contents.bytes)
			break;

		uint8_t blockHeader[blockHeaderBytes] = {};

		if (std::fread(blockHeader, 1, sizeof(blockHeader), source) != sizeof(blockHeader))
			break;

		if (std::memcmp(blockHeader, blockMagic, 4) != 0)
		{
			contents.stoppedEarly = true;
			contents.stoppedBecause = "block magic";

			break;
		}

		const uint16_t type = readU16(blockHeader + 4);
		const uint16_t version = readU16(blockHeader + 6);
		const uint32_t payloadBytes = readU32(blockHeader + 8);
		const uint32_t sequence = readU32(blockHeader + 12);
		const uint32_t expectCrc = readU32(blockHeader + 16);
		const uint32_t commit = readU32(blockHeader + 20);

		/* Not finished: the writer stopped between the payload and the commit
		   word. Everything before this is good, and this block is not. */
		if (commit != commitWord)
		{
			contents.stoppedEarly = true;
			contents.stoppedBecause = "uncommitted block";

			break;
		}

		if (sequence != expectedSequence)
		{
			contents.stoppedEarly = true;
			contents.stoppedBecause = "sequence gap";

			break;
		}

		const uint64_t end = static_cast<uint64_t>(at) + blockHeaderBytes + payloadBytes;

		if (end > contents.bytes)
		{
			contents.stoppedEarly = true;
			contents.stoppedBecause = "payload past the end of the file";

			break;
		}

		Block block;
		block.type = static_cast<BlockType>(type);
		block.version = version;
		block.sequence = sequence;
		block.payloadOffset = static_cast<uint64_t>(at) + blockHeaderBytes;
		block.raw.resize(payloadBytes);

		if (payloadBytes > 0 && std::fread(block.raw.data(), 1, payloadBytes, source) != payloadBytes)
		{
			contents.stoppedEarly = true;
			contents.stoppedBecause = "payload";

			break;
		}

		if (crc32(block.raw.data(), block.raw.size()) != expectCrc)
		{
			/* The bytes are there and are not the bytes that were written. A
			   block that failed its CRC is not half usable: nothing after it
			   can be trusted either, because nobody knows where it ended. */
			contents.stoppedEarly = true;
			contents.stoppedBecause = "crc";

			break;
		}

		block.payload = block.raw;
		contents.blocks.push_back(std::move(block));

		expectedSequence++;
	}

	std::fclose(source);

	if (contents.blocks.empty())
	{
		problem = Problem::NoBlocks;

		return false;
	}

	return true;
}

std::vector<uint8_t> RtosRecordingV3::packRounds(const std::vector<std::vector<uint8_t>>& rounds)
{
	std::vector<uint8_t> payload;

	uint8_t count[4] = {};
	putU32(count, static_cast<uint32_t>(rounds.size()));
	payload.insert(payload.end(), count, count + sizeof(count));

	for (const std::vector<uint8_t>& round : rounds)
	{
		uint8_t length[4] = {};
		putU32(length, static_cast<uint32_t>(round.size()));
		payload.insert(payload.end(), length, length + sizeof(length));
		payload.insert(payload.end(), round.begin(), round.end());
	}

	return payload;
}

bool RtosRecordingV3::unpackRounds(const std::vector<uint8_t>& payload, std::vector<std::vector<uint8_t>>& rounds)
{
	rounds.clear();

	if (payload.size() < 4)
		return false;

	const uint32_t count = readU32(payload.data());
	size_t at = 4;

	for (uint32_t index = 0; index < count; index++)
	{
		if (payload.size() < at + 4)
			return false;

		const uint32_t length = readU32(payload.data() + at);
		at += 4;

		if (payload.size() < at + length)
			return false;

		rounds.emplace_back(payload.begin() + static_cast<std::ptrdiff_t>(at),
		                    payload.begin() + static_cast<std::ptrdiff_t>(at + length));
		at += length;
	}

	return true;
}
