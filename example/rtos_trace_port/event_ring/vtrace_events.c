#include "vtrace_events.h"

#include "FreeRTOS.h"
#include "task.h"

#if __has_include("core_cm3.h")
#include "core_cm3.h"
#endif
volatile vtrace_event_ring_t vtrace_events;

#if VTRACE_TRANSPORT == VTRACE_TRANSPORT_RTT
	static char vtraceRttBuffer[VTRACE_RTT_BUFFER_SIZE];

	volatile vtrace_rtt_control_t vtrace_rtt_control = {
	    .identifier = "SEGGER RTT",
	    .upBuffers = 1,
	    .downBuffers = 0,
	    .up = {{.name = "trace", .buffer = vtraceRttBuffer, .size = VTRACE_RTT_BUFFER_SIZE, .writeOffset = 0, .readOffset = 0, .flags = 0}},
	};
#endif


/*
 * The time, in microseconds, from the kernel's tick plus the cycle counter.
 *
 * A tick is a millisecond in most builds, which is far too coarse to place a
 * switch: most switches happen inside one tick of each other. The cycle counter
 * has the resolution, the tick has the range, so the two are put together: the
 * tick says which millisecond, the counter's remainder says where inside it.
 *
 * Where the counter is not running - something has to switch it on, and a
 * firmware may not have - the answer is the tick alone, to the millisecond. It
 * is a worse answer than the cycle counter's and it is not a wrong one.
 */
uint32_t vtrace_now_us(void)
{
	const uint32_t milliseconds = (uint32_t)xTaskGetTickCount() * 1000u;

#if defined(DWT) && defined(CoreDebug)
	if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0u && (CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) != 0u)
	{
		const uint32_t perMicrosecond = (uint32_t)(configCPU_CLOCK_HZ / 1000000u);

		if (perMicrosecond > 0u)
			return milliseconds + ((DWT->CYCCNT / perMicrosecond) % 1000u);
	}
#endif

	return milliseconds;
}

/*
 * The record, on whichever carrier this build was told to use.
 *
 * The ring is filled either way - the "read the ring" path and the regression
 * tests use it, and it costs eight bytes a switch - and the streaming carriers
 * are written on top of it.
 *
 * Neither streaming carrier waits. A trace port that is not draining, or a
 * probe that has stopped reading, must not become a place the scheduler stops:
 * a record dropped is a gap the reader is told about, and a scheduler stopped
 * is a bug.
 */
static void vtrace_publish(const volatile vtrace_event_t* source)
{
#if VTRACE_TRANSPORT == VTRACE_TRANSPORT_ITM
	const uint8_t* bytes = (const uint8_t*)source;

	for (int index = 0; index < 8; index++)
	{
		volatile uint32_t* port = (volatile uint32_t*)(0xE0000000u + VTRACE_ITM_PORT * 4u);

		if (*port == 0u)
			break;

		*(volatile uint8_t*)port = bytes[index];
	}
#elif VTRACE_TRANSPORT == VTRACE_TRANSPORT_RTT
	vtrace_rtt_control_t* control = (vtrace_rtt_control_t*)&vtrace_rtt_control;
	vtrace_rtt_buffer_t* up = &control->up[0];
	const int32_t size = up->size;
	int32_t write = up->writeOffset;

	const uint8_t* bytes = (const uint8_t*)source;

	for (int32_t index = 0; index < 8; index++)
	{
		const int32_t next = (write + 1) % size;

		if (next == up->readOffset)
			break;

		up->buffer[write] = (char)bytes[index];
		write = next;
	}

	up->writeOffset = write;
#else
	(void)record;
#endif
}

/*
 * One record, then the counter.
 *
 * The counter is the commit: a reader copies the record and then looks at the
 * counter again, and throws the record away if the counter moved underneath it.
 * That is cheaper than any lock, and the reader is the one paying for it.
 */
void vtrace_event_switch(uint32_t tcb)
{
	/*
	 * The capacity is written here rather than in the object's initialiser,
	 * which the linker did not keep: the struct went to BSS and every field
	 * with it, so the host read a capacity of zero and had nothing to read the
	 * ring with. A compare against a constant costs three cycles on a path that
	 * runs a few thousand times a second, and this cannot be lost again.
	 */
	if (vtrace_events.capacity != VTRACE_EVENT_CAPACITY)
		vtrace_events.capacity = VTRACE_EVENT_CAPACITY;

	const uint32_t index = vtrace_events.head & (VTRACE_EVENT_CAPACITY - 1u);
	const uint32_t now = vtrace_now_us();

	vtrace_events.data[index].tcb = tcb;
	vtrace_events.data[index].timeUs = now;

	vtrace_events.head = vtrace_events.head + 1u;

	vtrace_publish(&vtrace_events.data[index]);
}
