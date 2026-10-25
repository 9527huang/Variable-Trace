#include "Rtec2Frames.hpp"

#include <cstring>

#include "Rtec2Decoder.hpp"

namespace
{
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

	uint16_t readU16(const uint8_t* at)
	{
		return static_cast<uint16_t>(at[0] | (at[1] << 8));
	}

	uint32_t readU32(const uint8_t* at)
	{
		return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) | (static_cast<uint32_t>(at[2]) << 16) |
		       (static_cast<uint32_t>(at[3]) << 24);
	}

	constexpr uint8_t syncFirst = 'R';
	constexpr uint8_t syncSecond = 'T';
	constexpr uint8_t frameEnd = 0x0A;
	constexpr uint8_t frameVersion = 2;
}  // namespace

uint32_t Rtec2FrameReader::crc32(const uint8_t* bytes, size_t size)
{
	if (!tableReady)
		buildTable();

	uint32_t crc = 0xFFFFFFFFu;

	for (size_t index = 0; index < size; index++)
		crc = (crc << 8) ^ table[((crc >> 24) ^ bytes[index]) & 0xFF];

	return crc;
}

void Rtec2FrameReader::feed(const uint8_t* bytes, size_t size)
{
	if (bytes == nullptr || size == 0)
		return;

	buffer.insert(buffer.end(), bytes, bytes + size);

	parse();
}

void Rtec2FrameReader::parse()
{
	size_t at = 0;

	while (at < buffer.size())
	{
		/* Find a sync. Everything before it is not part of any frame: a stream
		   joined in the middle, or the tail of one that failed its checks. */
		size_t sync = buffer.size();

		for (size_t index = at; index + 1 < buffer.size(); index++)
		{
			if (buffer[index] == syncFirst && buffer[index + 1] == syncSecond)
			{
				sync = index;
				break;
			}
		}

		if (sync == buffer.size())
		{
			/* No sync in what is held. Keep the last byte: it may be the 'R' of
			   a pair that arrives with the next read. */
			counters.resyncBytes += buffer.size() - at;

			const size_t keep = buffer.size() - at >= 1 ? 1 : 0;

			buffer.erase(buffer.begin(), buffer.end() - static_cast<std::ptrdiff_t>(keep));

			return;
		}

		if (sync > at)
		{
			counters.resyncBytes += sync - at;
			buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(sync));
			at = 0;
		}

		if (buffer.size() < headerBytes + 1)
			return; /* the rest of the header, or the end marker, is still coming */

		const uint8_t version = buffer[2];
		const uint8_t count = buffer[3];
		const uint16_t payloadBytes = readU16(&buffer[4]);
		const uint32_t crc = readU32(&buffer[6]);

		if (version != frameVersion || payloadBytes == 0 || payloadBytes > maxPayloadBytes)
		{
			/* Not a frame this build can use. Step past this sync and look for
			   the next one: the bytes in between were never a frame either. */
			counters.badLength++;
			counters.resyncBytes += 2;
			buffer.erase(buffer.begin(), buffer.begin() + 2);

			continue;
		}

		const size_t frameBytes = headerBytes + payloadBytes + 1;

		if (buffer.size() < frameBytes)
			return; /* wait for the rest */

		if (buffer[headerBytes + payloadBytes] != frameEnd)
		{
			counters.badEnd++;
			counters.resyncBytes += 2;
			buffer.erase(buffer.begin(), buffer.begin() + 2);

			continue;
		}

		const uint8_t* payload = buffer.data() + headerBytes;

		if (crc32(payload, payloadBytes) != crc)
		{
			counters.crcFailures++;
			counters.resyncBytes += 2;
			buffer.erase(buffer.begin(), buffer.begin() + 2);

			continue;
		}

		/* The payload has to hold exactly the records the header claims. A
		   frame with a record cut short, or with bytes after the last one, is
		   a frame whose writer and this reader disagree about the format. */
		const Rtec2DecodeResult decoded = Rtec2Decoder::decode(payload, payloadBytes);

		if (decoded.records.size() != count || decoded.truncatedBytes != 0)
		{
			counters.badPayload++;
			buffer.erase(buffer.begin(), buffer.begin() + 2);

			continue;
		}

		Rtec2Frame frame;

		frame.version = version;
		frame.count = count;
		frame.payload.assign(payload, payload + payloadBytes);

		ready.push_back(std::move(frame));
		counters.frames++;

		buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(frameBytes));
		at = 0;
	}
}

std::vector<Rtec2Frame> Rtec2FrameReader::take()
{
	std::vector<Rtec2Frame> out;

	out.swap(ready);

	return out;
}

void Rtec2FrameReader::clear()
{
	buffer.clear();
	ready.clear();
	counters = Stats{};
}
