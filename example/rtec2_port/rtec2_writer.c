#include "rtec2_writer.h"

/* Field offsets inside the control block, wire format §4.1. */
#define CONTROL_MAGIC 0u
#define CONTROL_WIRE 4u
#define CONTROL_SLOT_BYTES 6u
#define CONTROL_CAPACITY 8u
#define CONTROL_EPOCH_LOW 12u
#define CONTROL_EPOCH_HIGH 16u
#define CONTROL_TARGET_HZ 20u
#define CONTROL_HEAD 24u
#define CONTROL_TAIL 28u
#define CONTROL_GENERATION 32u
#define CONTROL_REJECTED 36u
#define CONTROL_OVERWRITTEN 40u
#define CONTROL_COMMITTED 44u
#define CONTROL_CAPABILITIES 48u
#define CONTROL_FAULT_COUNT 52u

/* A store that the compiler may not move across, which is what the ordering in
   §4.3 relies on. On a Cortex-M with a write buffer a data memory barrier is
   also needed before the head store; `__DMB()` from CMSIS does that, and this
   fallback keeps the file buildable without it. */
#if defined(__GNUC__)
#define RTEC2_BARRIER() __asm__ volatile("" ::: "memory")
#else
#define RTEC2_BARRIER() \
	do                 \
	{                  \
	} while (0)
#endif

static void putU16(volatile uint8_t* at, uint16_t value)
{
	at[0] = (uint8_t)(value & 0xFFu);
	at[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void putU32(volatile uint8_t* at, uint32_t value)
{
	at[0] = (uint8_t)(value & 0xFFu);
	at[1] = (uint8_t)((value >> 8) & 0xFFu);
	at[2] = (uint8_t)((value >> 16) & 0xFFu);
	at[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void putBytes(volatile uint8_t* at, const uint8_t* from, uint32_t count)
{
	uint32_t index = 0;

	for (index = 0; index < count; index++)
		at[index] = from[index];
}

static volatile uint8_t* slotAt(const rtec2_writer* writer, uint32_t sequence)
{
	return writer->blob + RTEC2_CONTROL_BYTES + (uint32_t)(sequence & (writer->capacity - 1u)) * RTEC2_SLOT_BYTES;
}

void rtec2_writer_init(rtec2_writer* writer, void* bytes, uint32_t capacity, uint32_t epochLow, uint32_t epochHigh, uint32_t targetHz,
                       uint32_t capabilities)
{
	volatile uint8_t* control = (volatile uint8_t*)bytes;
	uint32_t index = 0;

	writer->blob = control;
	writer->capacity = capacity;
	writer->nextSeq = 1; /* sequence 0 is reserved: it means "empty slot" */
	writer->rejected = 0;
	writer->overwritten = 0;
	writer->generation = 0;
	writer->overwrite = 1;

	/* The area is cleared so that a reader cannot mistake leftover bytes for a
	   record: a slot whose sequence is not the one being looked for is not
	   read, and zero is never a valid sequence. */
	for (index = 0; index < RTEC2_CONTROL_BYTES + capacity * RTEC2_SLOT_BYTES; index++)
		control[index] = 0;

	putU32(control + CONTROL_MAGIC, RTEC2_CONTROL_MAGIC);
	putU16(control + CONTROL_WIRE, RTEC2_WIRE_VERSION);
	putU16(control + CONTROL_SLOT_BYTES, RTEC2_SLOT_BYTES);
	putU32(control + CONTROL_CAPACITY, capacity);
	putU32(control + CONTROL_EPOCH_LOW, epochLow);
	putU32(control + CONTROL_EPOCH_HIGH, epochHigh);
	putU32(control + CONTROL_TARGET_HZ, targetHz);
	putU32(control + CONTROL_HEAD, 1);
	putU32(control + CONTROL_TAIL, 1);
	putU32(control + CONTROL_CAPABILITIES, capabilities);

	RTEC2_BARRIER();
}

rtec2_result rtec2_writer_emit(rtec2_writer* writer, const rtec2_event* event)
{
	volatile uint8_t* control = writer->blob;
	volatile uint8_t* slot = 0;
	uint32_t sequence = writer->nextSeq;
	uint32_t tail = 0;
	uint32_t used = 0;
	uint8_t flags = 0;

	if (event == 0 || event->type == 0)
	{
		writer->rejected++;
		putU32(control + CONTROL_REJECTED, writer->rejected);

		return RTEC2_REJECTED;
	}

	/* Twelve bytes is what a slot has room for after the header and the ticks;
	   a longer payload is refused rather than cut, because half a fault frame
	   is not a smaller fault frame. */
	if (event->payload != 0 && event->payloadBytes > 12)
	{
		writer->rejected++;
		putU32(control + CONTROL_REJECTED, writer->rejected);

		return RTEC2_REJECTED;
	}

	tail = (uint32_t)(control[CONTROL_TAIL] | (control[CONTROL_TAIL + 1] << 8) | (control[CONTROL_TAIL + 2] << 16) |
	                  ((uint32_t)control[CONTROL_TAIL + 3] << 24));

	rtec2_result result = RTEC2_OK;

	if (sequence - tail >= writer->capacity)
	{
		if (writer->overwrite == 0)
		{
			writer->rejected++;
			putU32(control + CONTROL_REJECTED, writer->rejected);

			return RTEC2_DROPPED;
		}

		writer->overwritten++;
		putU32(control + CONTROL_OVERWRITTEN, writer->overwritten);
		result = RTEC2_OK_OVERWROTE;
	}

	slot = slotAt(writer, sequence);

	/* Odd while a slot is being written, so that a reader watching the control
	   block can tell "busy" from "quiet" even before it looks at the slot. */
	writer->generation++;
	putU32(control + CONTROL_GENERATION, writer->generation);

	/* 1. Invalidate whatever was in this slot. */
	putU32(slot + 4, ~sequence);
	RTEC2_BARRIER();

	/* 2. The record body, in the same layout as the stream format. */
	flags = 0;

	if (event->hasTicks)
		flags |= 0x01u;

	if (event->payload != 0 && event->payloadBytes > 0)
		flags |= 0x02u;

	slot[8 + 0] = event->type;
	slot[8 + 1] = flags;
	putU16(slot + 8 + 2, 0);
	putU32(slot + 8 + 4, sequence);
	putU32(slot + 8 + 8, event->subject);
	putU32(slot + 8 + 12, event->object);
	putU32(slot + 8 + 16, event->arg0);
	putU32(slot + 8 + 20, event->arg1);

	used = 24;

	if (event->hasTicks)
	{
		putU32(slot + 8 + used, event->ticks);
		used += 4;
	}

	if ((flags & 0x02u) != 0)
	{
		slot[8 + used] = event->payloadBytes;
		used++;

		putBytes(slot + 8 + used, event->payload, event->payloadBytes);
		used += event->payloadBytes;
	}

	/* Nothing beyond the space the slot has; the reader would refuse it and the
	   whole batch would be thrown away because of one event. */
	if (used > 44u)
	{
		writer->rejected++;
		putU32(control + CONTROL_REJECTED, writer->rejected);

		return RTEC2_REJECTED;
	}

	RTEC2_BARRIER();

	/* 3. Close the sentinel pair. */
	putU32(slot + 52, ~sequence);
	RTEC2_BARRIER();

	/* 4. Give it its sequence: from here on a reader that checks all three
	   values will accept it. */
	putU32(slot, sequence);
	RTEC2_BARRIER();

	/* 5. Publish. The head is the only thing a reader needs to advance on, and
	   it moves last. */
	putU32(control + CONTROL_HEAD, sequence + 1);
	putU32(control + CONTROL_COMMITTED, sequence);

	writer->nextSeq = sequence + 1;

	writer->generation++;
	putU32(control + CONTROL_GENERATION, writer->generation);

	RTEC2_BARRIER();

	return result;
}

void rtec2_writer_next_epoch(rtec2_writer* writer, uint32_t epochLow, uint32_t epochHigh)
{
	volatile uint8_t* control = writer->blob;

	putU32(control + CONTROL_EPOCH_LOW, epochLow);
	putU32(control + CONTROL_EPOCH_HIGH, epochHigh);

	/* The head goes back to one, which is what a reader needs to see: a cursor
	   that is now ahead of it cannot be waiting for records that will never
	   come, and the reader treats that as a new run rather than as silence. */
	writer->nextSeq = 1;

	putU32(control + CONTROL_HEAD, 1);
	putU32(control + CONTROL_TAIL, 1);
	putU32(control + CONTROL_COMMITTED, 0);

	RTEC2_BARRIER();
}

void rtec2_dictionary_init(rtec2_dictionary* dictionary, void* bytes)
{
	volatile uint8_t* area = (volatile uint8_t*)bytes;
	uint32_t index = 0;

	dictionary->bytes = area;
	dictionary->nextSlot = 0;

	for (index = 0; index < RTEC2_DICTIONARY_SLOTS * RTEC2_DICTIONARY_NAME_BYTES; index++)
		area[index] = 0;
}

uint32_t rtec2_name_hash(const char* name)
{
	uint32_t hash = 2166136261u;

	if (name == 0)
		return hash;

	while (*name != '\0')
	{
		hash ^= (uint8_t)(*name);
		hash *= 16777619u;
		name++;
	}

	return hash;
}

uint16_t rtec2_dictionary_add(rtec2_dictionary* dictionary, const char* name)
{
	uint32_t length = 0;
	uint32_t index = 0;
	volatile uint8_t* at = 0;

	if (name == 0 || dictionary->nextSlot >= RTEC2_DICTIONARY_SLOTS)
		return 0xFFFFu;

	while (name[length] != '\0' && length < RTEC2_DICTIONARY_NAME_BYTES - 1u)
		length++;

	/* A name that does not fit is refused rather than cut: two tasks whose
	   names differ only after the fifteenth character would read as one. */
	if (name[length] != '\0')
		return 0xFFFFu;

	at = dictionary->bytes + dictionary->nextSlot * RTEC2_DICTIONARY_NAME_BYTES;

	/* Cleared first, so that a reader never sees half of an old name next to
	   half of a new one. */
	for (index = 0; index < RTEC2_DICTIONARY_NAME_BYTES; index++)
		at[index] = 0;

	for (index = 0; index < length; index++)
		at[index] = (uint8_t)name[index];

	RTEC2_BARRIER();

	return (uint16_t)dictionary->nextSlot++;
}
