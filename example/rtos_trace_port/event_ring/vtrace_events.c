#include "vtrace_events.h"

#include "FreeRTOS.h"
#include "task.h"

#if __has_include("core_cm3.h")
#include "core_cm3.h"
#endif
volatile vtrace_event_ring_t vtrace_events;


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
}
