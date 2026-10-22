/*
 * RTEC2 golden vector writer.
 *
 * This is the target side of the contract, written out as plain C with no
 * dependencies - the same shape a firmware would write: a fixed 24 byte header
 * and two optional tails. It exists so that the C++, the Python and the target
 * can be compared against one set of bytes rather than against three
 * interpretations of a document.
 *
 * Layout (docs/RTEC2-wire-format.md §3):
 *
 *   0   u8  type
 *   1   u8  flags       bit0 hasTargetTicks, bit1 hasPayload
 *   2   u16 reserved
 *   4   u32 sequence
 *   8   u32 subject
 *  12   u32 object
 *  16   u32 arg0
 *  20   u32 arg1
 *  24   u32 targetTicks   only when flags.bit0
 *  28   u8  payloadBytes  only when flags.bit1
 *  29   ..  payload
 */

#ifndef RTEC2_WRITER_H
#define RTEC2_WRITER_H

#include <stddef.h>

#define RTEC2_FLAG_TICKS 0x01u
#define RTEC2_FLAG_PAYLOAD 0x02u

typedef struct rtec2_record
{
	unsigned char type;
	unsigned char hasTicks;
	unsigned ticks;
	unsigned sequence;
	unsigned subject;
	unsigned object;
	unsigned arg0;
	unsigned arg1;

	const unsigned char* payload;
	unsigned char payloadBytes;
} rtec2_record;

static void rtec2_put_u32(unsigned char* out, unsigned value)
{
	out[0] = (unsigned char)(value & 0xFFu);
	out[1] = (unsigned char)((value >> 8) & 0xFFu);
	out[2] = (unsigned char)((value >> 16) & 0xFFu);
	out[3] = (unsigned char)((value >> 24) & 0xFFu);
}

/* Writes one record into `out` and returns how many bytes it took. The caller
 * owns the buffer and is expected to have room for 41 bytes (the largest a
 * record can be: header, ticks and a full 12 byte payload). */
static size_t rtec2_write(unsigned char* out, const rtec2_record* record)
{
	size_t at = 24;
	unsigned char flags = 0;

	if (record->hasTicks)
		flags |= RTEC2_FLAG_TICKS;

	if (record->payload != NULL && record->payloadBytes > 0)
		flags |= RTEC2_FLAG_PAYLOAD;

	out[0] = record->type;
	out[1] = flags;
	out[2] = 0;
	out[3] = 0;

	rtec2_put_u32(out + 4, record->sequence);
	rtec2_put_u32(out + 8, record->subject);
	rtec2_put_u32(out + 12, record->object);
	rtec2_put_u32(out + 16, record->arg0);
	rtec2_put_u32(out + 20, record->arg1);

	if (record->hasTicks)
	{
		rtec2_put_u32(out + at, record->ticks);
		at += 4;
	}

	if (record->payload != NULL && record->payloadBytes > 0)
	{
		unsigned char index = 0;

		out[at] = record->payloadBytes;
		at++;

		for (index = 0; index < record->payloadBytes; index++)
			out[at + index] = record->payload[index];

		at += record->payloadBytes;
	}

	return at;
}

#endif
