#include "rtec2_stream.h"

/* The ITM's registers, at the addresses the architecture fixes. */
#define ITM_STIM_BASE 0xE0000000u
#define ITM_TER (*(volatile uint32_t*)0xE0000E00u)
#define ITM_TCR (*(volatile uint32_t*)0xE0000E80u)
#define ITM_LAR (*(volatile uint32_t*)0xE0000FB0u)

/* The record flags, wire format §3. */
#define RTEC2_FLAG_TICKS 0x01u
#define RTEC2_FLAG_PAYLOAD 0x02u

/* CRC-32/MPEG-2, nibble at a time. A full 256-entry table is a kilobyte of
   flash for the same answer; this is 64 bytes and about twice the work, which
   at one frame every twenty milliseconds is nothing. */
static const uint32_t crcNibble[16] = {0x00000000u, 0x04C11DB7u, 0x09823B6Eu, 0x0D4326D9u, 0x130476DCu, 0x17C56B6Bu, 0x1A864DB2u,
                                       0x1E475005u, 0x2608EDB8u, 0x22C9F00Fu, 0x2F8AD6D6u, 0x2B4BCB61u, 0x350C9B64u, 0x31CD86D3u,
                                       0x3C8EA00Au, 0x384FBDBDu};

static uint32_t crc32_mpeg2(const uint8_t* bytes, uint32_t size)
{
	uint32_t crc = 0xFFFFFFFFu;

	for (uint32_t index = 0; index < size; index++)
	{
		crc ^= (uint32_t)bytes[index] << 24;

		crc = (crc << 4) ^ crcNibble[(crc >> 28) & 0x0Fu];
		crc = (crc << 4) ^ crcNibble[(crc >> 28) & 0x0Fu];
	}

	return crc;
}

/* Words on the stimulus port.
 *
 * The size of the store is what decides the packet: an 8 bit store is a one
 * byte packet and a 32 bit store is a four byte packet. Per byte that is two
 * wire bytes against one and a quarter, and with a timestamp beside each
 * packet the difference is what decides whether the wire keeps up at all - a
 * measured run lost 96% of its payload writing bytes one at a time.
 *
 * The tail is padded with zeros. Everything after the end marker is ignored by
 * the reader, and a padded word costs less than a second packet. */
static void itm_word(uint32_t port, uint32_t value)
{
	*(volatile uint32_t*)(ITM_STIM_BASE + port * 4u) = value;
}

static void put16(uint8_t* at, uint16_t value)
{
	at[0] = (uint8_t)(value & 0xFFu);
	at[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void put32(uint8_t* at, uint32_t value)
{
	at[0] = (uint8_t)(value & 0xFFu);
	at[1] = (uint8_t)((value >> 8) & 0xFFu);
	at[2] = (uint8_t)((value >> 16) & 0xFFu);
	at[3] = (uint8_t)((value >> 24) & 0xFFu);
}

void rtec2_stream_enable_itm(uint32_t port)
{
	/* Unlock, enable the unit and the port, then drop the lock. A debugger that
	   connected after boot will have cleared all of this. */
	ITM_LAR = 0xC5ACCE55u;
	ITM_TCR |= 1u;
	ITM_TER |= (1u << port);
}

void rtec2_stream_init(rtec2_stream* stream, void* bytes, uint32_t capacity, uint32_t port)
{
	if (stream == 0)
		return;

	stream->bytes = (volatile uint8_t*)bytes;
	stream->capacity = capacity > RTEC2_STREAM_MAX_PAYLOAD ? RTEC2_STREAM_MAX_PAYLOAD : capacity;
	stream->port = port;
	stream->used = 0;
	stream->count = 0;
	stream->nextSeq = 1u;
	stream->frames = 0;
	stream->records = 0;
	stream->dropped = 0;
	stream->enabled = 1u;
}

/* How many bytes one record takes, or zero when it cannot be written. */
static uint32_t record_bytes(const rtec2_event* event)
{
	if (event->type == 0u)
		return 0;

	if (event->payload != 0 && event->payloadBytes > 12u)
		return 0;

	return 24u + (event->hasTicks != 0u ? 4u : 0u) + (event->payload != 0 && event->payloadBytes > 0u ? 1u + event->payloadBytes : 0u);
}

static void stage_record(rtec2_stream* stream, const rtec2_event* event)
{
	uint8_t* at = (uint8_t*)stream->bytes + RTEC2_STREAM_HEADER_BYTES + stream->used;
	const uint8_t hasPayload = (uint8_t)(event->payload != 0 && event->payloadBytes > 0u);

	at[0] = event->type;
	at[1] = (uint8_t)((event->hasTicks != 0u ? RTEC2_FLAG_TICKS : 0u) | (hasPayload != 0u ? RTEC2_FLAG_PAYLOAD : 0u));
	put16(at + 2, 0u);
	put32(at + 4, stream->nextSeq);
	put32(at + 8, event->subject);
	put32(at + 12, event->object);
	put32(at + 16, event->arg0);
	put32(at + 20, event->arg1);

	uint32_t offset = 24u;

	if (event->hasTicks != 0u)
	{
		put32(at + offset, event->ticks);
		offset += 4u;
	}

	if (hasPayload != 0u)
	{
		at[offset] = event->payloadBytes;
		offset += 1u;

		for (uint8_t index = 0; index < event->payloadBytes; index++)
			at[offset + index] = event->payload[index];

		offset += event->payloadBytes;
	}

	stream->used += offset;
	stream->count++;
	stream->nextSeq++;
	stream->records++;
}

void rtec2_stream_flush(rtec2_stream* stream)
{
	if (stream == 0 || stream->used == 0u || stream->count == 0u || stream->enabled == 0u)
	{
		if (stream != 0)
		{
			stream->used = 0;
			stream->count = 0;
		}

		return;
	}

	volatile uint8_t* frame = stream->bytes;

	frame[0] = 'R';
	frame[1] = 'T';
	frame[2] = 2u; /* frameVersion */
	frame[3] = (uint8_t)stream->count;
	put16((uint8_t*)frame + 4, (uint16_t)stream->used);
	put32((uint8_t*)frame + 6, crc32_mpeg2((const uint8_t*)frame + RTEC2_STREAM_HEADER_BYTES, stream->used));

	/* The end marker is written into the buffer before the loop, so that the
	   whole frame - marker included - goes out in whole words. */
	frame[RTEC2_STREAM_HEADER_BYTES + stream->used] = 0x0Au;

	const uint32_t total = RTEC2_STREAM_HEADER_BYTES + stream->used + 1u;

	for (uint32_t index = 0; index < total; index += 4u)
	{
		uint32_t word = 0;

		for (uint32_t byte = 0; byte < 4u; byte++)
			word |= (uint32_t)(index + byte < total ? frame[index + byte] : 0u) << (byte * 8u);

		itm_word(stream->port, word);
	}

	stream->frames++;
	stream->used = 0;
	stream->count = 0;
}

rtec2_result rtec2_stream_emit(rtec2_stream* stream, const rtec2_event* event)
{
	if (stream == 0 || event == 0)
		return RTEC2_REJECTED;

	if (stream->enabled == 0u)
		return RTEC2_DROPPED;

	const uint32_t needed = record_bytes(event);

	if (needed == 0u)
	{
		stream->dropped++;

		return RTEC2_REJECTED;
	}

	/* Flush rather than cut: a record is staged whole or not at all, and the
	   frame it was going into is complete either way. */
	if (stream->used + needed > stream->capacity)
		rtec2_stream_flush(stream);

	if (stream->used + needed > stream->capacity)
	{
		/* One record larger than a whole frame. Refused by the writer's own
		   rules, not by the buffer's. */
		stream->dropped++;

		return RTEC2_DROPPED;
	}

	stage_record(stream, event);

	return RTEC2_OK;
}
