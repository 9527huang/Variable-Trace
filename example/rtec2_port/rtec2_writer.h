/*
 * RTEC2 writer - the target side of docs/RTEC2-wire-format.md.
 *
 * This is a reference implementation, not a library: it is meant to be read,
 * copied into a firmware and adapted to whatever the hook macros look like
 * there. It has no dependencies beyond fixed width integers, no allocation, and
 * no waiting - an event that cannot be written is counted and dropped, because
 * a debug tracer that stalls the scheduler is a tracer that changes what it
 * measures.
 *
 * Layout (all little endian):
 *
 *   blob[0 .. 63]                control block
 *   blob[64 + i * 56 .. ]         slot i, i & (capacity - 1)
 *
 * A slot is written in five steps, and the order is the whole mechanism:
 *
 *   1. seqCheck = ~seq          invalidates whatever was there
 *   2. the record body
 *   3. tailCheck = ~seq
 *   4. seq = seq
 *   5. head = seq + 1           the record becomes readable
 *
 * A reader that sees a slot mid-write fails steps 1 or 3 or 4 and takes nothing
 * from that batch - which is the right answer, because the alternative is a
 * cursor that has moved past records nobody counted.
 */

#ifndef RTEC2_WRITER_H
#define RTEC2_WRITER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define RTEC2_CONTROL_BYTES 64u
#define RTEC2_SLOT_BYTES 56u

/* 'R','T','C','2' read as a little endian word. */
#define RTEC2_CONTROL_MAGIC 0x32435452u

#define RTEC2_WIRE_VERSION 2u

/* Capability bits, wire format §6. */
#define RTEC2_CAP_RAW_HANDLE32 (1u << 0)
#define RTEC2_CAP_LIFECYCLE (1u << 1)
#define RTEC2_CAP_GENERATION (1u << 2)
#define RTEC2_CAP_TARGET_CLOCK (1u << 3)
#define RTEC2_CAP_EVENT_SEQUENCE (1u << 4)
#define RTEC2_CAP_LOSS_DETECTION (1u << 5)
#define RTEC2_CAP_ISR_EVENTS (1u << 6)
#define RTEC2_CAP_FAULT_EVENTS (1u << 7)
#define RTEC2_CAP_SYNC_EVENTS (1u << 8)
#define RTEC2_CAP_HEAP_EVENTS (1u << 9)
#define RTEC2_CAP_DICTIONARY (1u << 10)

/* One event, as the caller hands it over. */
typedef struct rtec2_event
{
	uint8_t type;

	/* The target's counter at the moment it happened. Zero means "no clock",
	   and the writer will not invent one. */
	uint8_t hasTicks;
	uint32_t ticks;

	uint32_t subject;
	uint32_t object;
	uint32_t arg0;
	uint32_t arg1;

	/* Up to 12 bytes. Anything longer is refused, not truncated: half a fault
	   frame is worse than none. */
	const uint8_t* payload;
	uint8_t payloadBytes;
} rtec2_event;

/* What a write did. */
typedef enum rtec2_result
{
	RTEC2_OK = 0,
	/* Written, but it landed on a slot the reader has not taken yet. */
	RTEC2_OK_OVERWROTE = 1,
	/* Not written: the payload is too large or the type is zero. */
	RTEC2_REJECTED = 2,
	/* Not written: the ring is full and the policy is to drop rather than
	   overwrite. */
	RTEC2_DROPPED = 3,
} rtec2_result;

typedef struct rtec2_writer
{
	/* The whole area: control block followed by `capacity` slots. */
	volatile uint8_t* blob;
	uint32_t capacity;

	/* The next sequence to write. A mirror of the control block's head, kept
	   here so that the writer does not have to read back its own memory. */
	uint32_t nextSeq;

	uint32_t rejected;
	uint32_t overwritten;
	uint32_t generation;

	/* Non-zero: a full ring overwrites its oldest record (what a ring is for).
	   Zero: it drops the new one instead. */
	uint8_t overwrite;
} rtec2_writer;

/*
 * Prepares the area and writes the control block.
 *
 * `bytes` must be at least RTEC2_CONTROL_BYTES + capacity * RTEC2_SLOT_BYTES.
 * `capacity` must be a power of two and at least 16 - the reader refuses
 * anything else, and a writer that accepts it only produces a file nobody can
 * read.
 */
void rtec2_writer_init(rtec2_writer* writer, void* bytes, uint32_t capacity, uint32_t epochLow, uint32_t epochHigh, uint32_t targetHz,
                       uint32_t capabilities);

/*
 * Writes one event.
 *
 * Safe to call from an interrupt as long as it is the only writer: the steps
 * are ordered and each one is a single word store, and there is no loop that
 * depends on anything but the event itself.
 */
rtec2_result rtec2_writer_emit(rtec2_writer* writer, const rtec2_event* event);

/* Starts a new epoch: the sequence starts again at one and the head goes back,
   which is what tells a reader that its cursor no longer means anything. */
void rtec2_writer_next_epoch(rtec2_writer* writer, uint32_t epochLow, uint32_t epochHigh);

/* A named object, for the dictionary (§7). Returns the slot it went into, or
   0xFFFF when the name is too long or the area is full. */
#define RTEC2_DICTIONARY_SLOTS 128u
#define RTEC2_DICTIONARY_NAME_BYTES 16u

typedef struct rtec2_dictionary
{
	volatile uint8_t* bytes;
	uint32_t nextSlot;
} rtec2_dictionary;

void rtec2_dictionary_init(rtec2_dictionary* dictionary, void* bytes);
uint16_t rtec2_dictionary_add(rtec2_dictionary* dictionary, const char* name);

/* FNV-1a, the hash the name reference carries. */
uint32_t rtec2_name_hash(const char* name);

#ifdef __cplusplus
}
#endif

#endif
