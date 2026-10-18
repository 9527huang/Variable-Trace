#include "RtosItmDecoder.hpp"

#include <cstring>

void RtosItmDecoder::feed(const uint8_t* bytes, size_t size)
{
	if (bytes == nullptr)
		return;

	for (size_t index = 0; index < size; index++)
		consume(bytes[index]);
}

void RtosItmDecoder::consume(uint8_t byte)
{
	/* The rest of the packet being read. */
	if (remaining > 0)
	{
		if (collecting)
			recordBytes.push_back(byte);

		remaining--;

		return;
	}

	/* Inside a timestamp or a packet this decoder does not name: its bytes run
	   on while the continuation bit stays set. */
	if (skipping)
	{
		skipping = (byte & 0x80) != 0;

		return;
	}

	/* The order of these three matters, and it is the order the trace view's
	   own decoder tests them in. A timestamp has a zero low nibble and a
	   non-zero middle - which a naive "low bits are zero, nothing follows"
	   rule reads as a sync byte, and then the timestamp's own payload is read
	   as a header and the rest of the stream is noise. */
	if (byte == 0x70)
		return;                                          /* overflow: nothing follows */

	if ((byte & 0x03) != 0 && (byte & 0x04) == 0)        /* a software-source packet */
	{
		collecting = ((byte & 0xf8) >> 3) == recordPort;
		remaining = (byte & 0x03) == 0x03 ? 4 : (byte & 0x03);

		return;
	}

	if ((byte & 0x0f) == 0 && (byte & 0x70) != 0)       /* local time: its own bytes follow */
	{
		skipping = (byte & 0x80) != 0;

		return;
	}

	/* A source packet whose low bits are not a size this decoder knows, and
	   anything else: step over it while the continuation bit says to. */
	if ((byte & 0x03) != 0 || (byte & 0x0b) == 0x08)     /* extension */
	{
		skipping = (byte & 0x80) != 0;

		return;
	}

	/* A sync byte, or a zero between packets: nothing follows it. */
}

std::vector<RtosEventRing::Record> RtosItmDecoder::take()
{
	std::vector<RtosEventRing::Record> records;

	const size_t whole = recordBytes.size() / sizeof(RtosEventRing::Record);

	if (whole == 0)
		return records;

	records.reserve(whole);

	for (size_t index = 0; index < whole; index++)
	{
		RtosEventRing::Record record;

		std::memcpy(&record.tcbAddress, recordBytes.data() + index * sizeof(RtosEventRing::Record), 4);
		std::memcpy(&record.timeUs, recordBytes.data() + index * sizeof(RtosEventRing::Record) + 4, 4);

		records.push_back(record);
	}

	recordBytes.erase(recordBytes.begin(), recordBytes.begin() + static_cast<long>(whole * sizeof(RtosEventRing::Record)));

	return records;
}

void RtosItmDecoder::clear()
{
	recordBytes.clear();
	remaining = 0;
	collecting = false;
	skipping = false;
}
