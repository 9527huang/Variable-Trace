/*
 * A ring of task switches, written by the target and read by the host.
 *
 * This is path B of the task view: instead of sampling the kernel twice a
 * second and drawing what each sample found, the target records the moment each
 * task was switched in. The host then has the switch times themselves, which is
 * the difference between "task X was running at 1.20 s" and "task X ran from
 * 1.20 s to 1.27 s".
 *
 * Eight bytes a record, and the write is two stores and a counter: it runs
 * inside the scheduler's critical section, on the context switch path, so it
 * has to cost about nothing. Nothing is formatted, nothing is allocated, and
 * there is no state machine to get wrong.
 *
 * The ring overwrites its oldest record when it fills. A debug stream should
 * lose its beginning rather than its end - the reader is looking at what just
 * happened - and the host can tell, because the counter it keeps is monotonic
 * and the ring holds only so many.
 *
 * The names below are the interface the host looks up by symbol. They are
 * `volatile` because the target writes them while the host reads.
 */
#ifndef _VTRACE_EVENTS_H
#define _VTRACE_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Records hold: the control block address of the task switched in, and the time
   in microseconds since the target started. */
typedef struct
{
	uint32_t tcb;
	uint32_t timeUs;
} vtrace_event_t;

/*
 * Where the records go.
 *
 * The record is the same eight bytes whichever way it travels; only the
 * carrier changes.
 *
 *   RING  the target's memory, read by the host between rounds. No pins, no
 *         extra hardware, no protocol; loses whatever the ring overwrote
 *         between two reads. The default.
 *
 *   ITM   the stimulus port of the trace unit, carried over SWO. Needs the
 *         trace pin wired and the trace clock set up, and carries every record
 *         in order with no ring to wrap - at the cost of a wire.
 *
 *   RTT   an up-buffer in RAM that the probe drains by itself. No pins and no
 *         polling schedule of the host's; the cost is a control block in RAM
 *         that the probe has to find.
 */
#define VTRACE_TRANSPORT_RING 0
#define VTRACE_TRANSPORT_ITM  1
#define VTRACE_TRANSPORT_RTT  2

#ifndef VTRACE_TRANSPORT
#define VTRACE_TRANSPORT VTRACE_TRANSPORT_RING
#endif

/* The stimulus port the records go out on, when the carrier is the ITM. It has
   to be one the reader has enabled. */
#ifndef VTRACE_ITM_PORT
#define VTRACE_ITM_PORT 1
#endif

/* The up-buffer's size, when the carrier is RTT. */
#ifndef VTRACE_RTT_BUFFER_SIZE
#define VTRACE_RTT_BUFFER_SIZE 1024
#endif

/* How many records the ring holds. A power of two, so that the index is a mask
   rather than a division on the switch path. At 8 bytes a record this is 16 KiB
   for 2048 switches, which at a few thousand switches a second is a couple of
   seconds of the most recent history. */
#ifndef VTRACE_EVENT_CAPACITY
#define VTRACE_EVENT_CAPACITY 2048u
#endif

#if (VTRACE_EVENT_CAPACITY & (VTRACE_EVENT_CAPACITY - 1u)) != 0
#error "VTRACE_EVENT_CAPACITY must be a power of two"
#endif

#if VTRACE_TRANSPORT == VTRACE_TRANSPORT_RTT
/*
 * The control block the probe looks for, in the layout SEGGER's RTT uses.
 *
 * Written by hand rather than pulled in with SEGGER's library: what is needed
 * here is one up-buffer and no down-buffers. The probe finds it by the
 * identifier, which must be the first sixteen bytes and spelled exactly.
 */
typedef struct
{
	const char* name;
	char* buffer;
	int32_t size;
	int32_t writeOffset;
	int32_t readOffset;
	int32_t flags;
} vtrace_rtt_buffer_t;

typedef struct
{
	char identifier[16];
	int32_t upBuffers;
	int32_t downBuffers;
	vtrace_rtt_buffer_t up[3];
	vtrace_rtt_buffer_t down[3];
} vtrace_rtt_control_t;

extern volatile vtrace_rtt_control_t vtrace_rtt_control;
#endif

/*
 * The ring, in one object.
 *
 * One symbol rather than three, because the linker drops what nothing in the
 * target refers to, and the host needs all of it. `head` and `data` are written
 * by the switch hook; `capacity` is read only by the host, and would be gone
 * from the image on its own. Everything is 32-bit and in this order, so the
 * host reads the three at fixed offsets from one address.
 *
 * Head counts records written and is monotonic: a reader that keeps its own
 * copy can tell how many it missed.
 */
typedef struct
{
	volatile uint32_t head;
	volatile uint32_t capacity;
	volatile vtrace_event_t data[VTRACE_EVENT_CAPACITY];
} vtrace_event_ring_t;

extern volatile vtrace_event_ring_t vtrace_events;

/* Records the switch that just happened. Called from `traceTASK_SWITCHED_IN()`,
   inside the scheduler's critical section, so it must not block or allocate. */
void vtrace_event_switch(uint32_t tcb);

/* Milliseconds times a thousand becomes microseconds, and clears at about
   seventy-one minutes. A session that long is not what this is for; a reader
   that cares can tell, because the counter goes backwards by more than half its
   range when it wraps. */
uint32_t vtrace_now_us(void);

#ifdef __cplusplus
}
#endif

#endif
