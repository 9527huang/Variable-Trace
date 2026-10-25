/*
 * RTEC2 over a byte stream: frames of records, written to the ITM.
 *
 * The ring gives the host slots it can address. A stream gives it bytes, and
 * the only thing that restores the record boundaries is a frame: 'R','T', a
 * version, a count, a length, a CRC over the payload, the payload, and a 0x0A
 * that says the frame really ended.
 *
 * Why a stream at all: the ring has to be read between rounds, and what the
 * writer overwrote in the meantime is gone. A stream has no such gap - every
 * record goes out in order - at the cost of a wire.
 *
 * Two rules shape the code:
 *
 * - **Nothing here waits.** A write to a full ITM FIFO is dropped by the
 *   hardware; there is no back pressure to honour and no room to yield from an
 *   interrupt. A frame that cannot go out is counted.
 * - **A frame is written as it will be read.** The CRC covers what is sent,
 *   the end marker comes last, and a frame that is cut off by a reset simply
 *   fails its checks at the host - which is the point of the marker.
 *
 * Records are staged in RAM and flushed as one frame rather than sent one at a
 * time: 30 bytes of record become 30 bytes of frame plus ten, where a packet
 * per byte would double it on the wire.
 */

#ifndef RTEC2_STREAM_H
#define RTEC2_STREAM_H

#include <stdint.h>

#include "rtec2_writer.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* The ITM stimulus port the frames go out on. It has to be a port the reader
   enabled; the trace view uses the low channels for its own packets. */
#ifndef RTEC2_STREAM_PORT
#define RTEC2_STREAM_PORT 1
#endif

/* Frame header, as docs/RTEC2-wire-format.md §2.1 writes it. */
#define RTEC2_STREAM_HEADER_BYTES 10u

/* Wire format §2.1: a longer frame is not a frame. */
#define RTEC2_STREAM_MAX_PAYLOAD 1024u

typedef struct rtec2_stream
{
	/* The staging area: header, payload, end marker. */
	volatile uint8_t* bytes;
	uint32_t capacity;
	uint32_t port;

	uint32_t used;
	uint32_t count;
	uint32_t nextSeq;

	uint32_t frames;
	uint32_t records;
	uint32_t dropped;

	/* Non-zero: writes go to the ITM. Zero: they go nowhere, so a build with
	   the stream compiled in but switched off costs one test per record. */
	uint8_t enabled;
} rtec2_stream;

/*
 * Prepares the staging area.
 *
 * `bytes` must hold RTEC2_STREAM_HEADER_BYTES + capacity + 1. `capacity` is the
 * payload bytes one frame may carry; anything over RTEC2_STREAM_MAX_PAYLOAD is
 * clamped, because the host refuses those and a frame nobody can read is not a
 * frame.
 */
void rtec2_stream_init(rtec2_stream* stream, void* bytes, uint32_t capacity, uint32_t port);

/*
 * Stages one record, and flushes when the next one would not fit.
 *
 * Safe from an interrupt, as long as it is the only writer - the same rule the
 * ring writer follows, for the same reason.
 */
rtec2_result rtec2_stream_emit(rtec2_stream* stream, const rtec2_event* event);

/* Sends what is staged. Called by the tick, and by `emit` when it is full: a
   record staged and never flushed is a record nobody sees. */
void rtec2_stream_flush(rtec2_stream* stream);

/*
 * Turns the ITM on for this port, and leaves it on.
 *
 * A debugger connect clears the ITM's configuration, so this cannot be done
 * once at boot and trusted: the host programs the same registers for the same
 * reason, and whichever runs later wins. Calling it every flush is one compare.
 */
void rtec2_stream_enable_itm(uint32_t port);

#ifdef __cplusplus
}
#endif

#endif
