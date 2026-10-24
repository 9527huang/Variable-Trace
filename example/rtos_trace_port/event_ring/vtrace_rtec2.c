#include "vtrace_rtec2.h"

#include "rtec2_writer.h"

#include "FreeRTOS.h"
#include "task.h"

/* Wire types, as the decoder on the host reads them. Spelled here rather than
   pulled from a shared header: the target is a separate program that may be
   compiled a year from now, and one line of numbers is cheaper to keep in step
   than a header that has to be found on an include path. */
#define VTRACE_TYPE_WAIT_BEGIN 0x22u
#define VTRACE_TYPE_SIGNAL 0x24u
#define VTRACE_TYPE_TASK_SWITCH_IN 0x13u
#define VTRACE_TYPE_ISR_ENTER 0x30u
#define VTRACE_TYPE_ISR_EXIT 0x31u
#define VTRACE_TYPE_FAULT 0x40u
#define VTRACE_TYPE_ALLOC 0x50u
#define VTRACE_TYPE_FREE 0x51u

/* The one symbol the host reads. Flat on purpose - see the header. */
uint8_t vtrace_rtec2_blob[RTEC2_CONTROL_BYTES + VTRACE_RTEC2_CAPACITY * RTEC2_SLOT_BYTES];

static rtec2_writer vtraceWriter;

/*
 * The target's clock, in microseconds.
 *
 * The kernel tick is a millisecond, and most of what this records happens
 * inside one tick of the next thing: three switches and an interrupt can share
 * a millisecond. The cycle counter has the resolution and the tick has the
 * range, so the tick is the whole milliseconds and the counter's remainder is
 * the fraction of one.
 *
 * Where nothing has started the cycle counter - a firmware that does not touch
 * DWT, which is allowed - this returns milliseconds. That is a worse answer and
 * not a wrong one: the reader sees a coarser axis, not a broken one.
 */
static uint32_t vtrace_now_us(void)
{
	const uint32_t milliseconds = (uint32_t)xTaskGetTickCount() * 1000u;

	const uint32_t perMicrosecond = (uint32_t)(configCPU_CLOCK_HZ / 1000000u);

	if (perMicrosecond > 0u)
	{
		/* 0xE0001004 is DWT->CYCCNT and 0xE000EDFC is CoreDebug->DEMCR, on
		   every Cortex-M. Written as addresses for the same reason the ITM
		   registers were: this file must compile without a device header. */
		const uint32_t demcr = *(volatile uint32_t*)0xE000EDFCu;
		const uint32_t cyccnt = *(volatile uint32_t*)0xE0001004u;

		if ((demcr & (1u << 24)) != 0u)
			return milliseconds + ((cyccnt / perMicrosecond) % 1000u);
	}

	return milliseconds;
}

/* The same clock, asked the way an interrupt is allowed to ask.
 *
 * `vtrace_now_us` calls `xTaskGetTickCount`, and this timer runs at the highest
 * priority unless something lowers it. Calling the scheduler from above
 * `configMAX_SYSCALL_INTERRUPT_PRIORITY` is exactly the situation FreeRTOS
 * documents as undefined, and on this target it showed up as a tracer that ran
 * for a few minutes and then stopped writing with the CPU still spinning in an
 * interrupt. The kernel has a call for this context; use it. */
static uint32_t vtrace_now_us_from_isr(void)
{
	const uint32_t milliseconds = (uint32_t)xTaskGetTickCountFromISR() * 1000u;

	const uint32_t perMicrosecond = (uint32_t)(configCPU_CLOCK_HZ / 1000000u);

	if (perMicrosecond > 0u)
	{
		const uint32_t demcr = *(volatile uint32_t*)0xE000EDFCu;
		const uint32_t cyccnt = *(volatile uint32_t*)0xE0001004u;

		if ((demcr & (1u << 24)) != 0u)
			return milliseconds + ((cyccnt / perMicrosecond) % 1000u);
	}

	return milliseconds;
}

static void emit(uint8_t type, uint32_t subject, uint32_t object, uint32_t arg0, uint32_t arg1)
{
	rtec2_event event = {0};

	event.type = type;
	event.hasTicks = 1;
	event.ticks = vtrace_now_us();
	event.subject = subject;
	event.object = object;
	event.arg0 = arg0;
	event.arg1 = arg1;

	(void)rtec2_writer_emit(&vtraceWriter, &event);
}

void vtrace_rtec2_init(void)
{
	/* The capabilities say what this firmware actually sends, and the host
	   shows "this source does not carry it" for everything it does not claim.
	   Claiming a bit that never produces an event would turn an empty track
	   into a lie about the carrier. */
	const uint32_t capabilities = RTEC2_CAP_RAW_HANDLE32 | RTEC2_CAP_EVENT_SEQUENCE | RTEC2_CAP_LOSS_DETECTION | RTEC2_CAP_TARGET_CLOCK |
	                              RTEC2_CAP_ISR_EVENTS | RTEC2_CAP_FAULT_EVENTS | RTEC2_CAP_SYNC_EVENTS | RTEC2_CAP_HEAP_EVENTS;

	vtraceWriter.overwrite = 1;

	/* A million, not the CPU frequency: `targetHz` describes the unit the ticks
	   are in, and the ticks are microseconds. Declaring eight million here made
	   every event arrive with a time eight times too small - or, once the reader
	   worked out the scale did not fit, arrive with no time at all. */
	rtec2_writer_init(&vtraceWriter, vtrace_rtec2_blob, VTRACE_RTEC2_CAPACITY, 1u, 0u, 1000000u, capabilities);
}

void vtrace_rtec2_switch(uint32_t tcb)
{
	emit(VTRACE_TYPE_TASK_SWITCH_IN, tcb, 0u, 0u, 0u);
}

void vtrace_rtec2_isr_enter(void)
{
	emit(VTRACE_TYPE_ISR_ENTER, 0u, 0u, 0u, 0u);
}

void vtrace_rtec2_isr_exit(void)
{
	emit(VTRACE_TYPE_ISR_EXIT, 0u, 0u, 0u, 0u);
}

/*
 * TIM2, on the STM32F103 this firmware is built for.
 *
 * The addresses are written out for the same reason the ITM ones were: no
 * device header is included anywhere in this firmware, and three registers are
 * cheaper to spell than to pull a vendor header in for.
 *
 * Twenty milliseconds, not one: the interrupt exists to give the ISR track
 * something true to draw, not to load the CPU. Fifty pairs a second is enough
 * for a band to have texture and small enough that the ring is never the reason
 * a switch went missing.
 */
#define VTRACE_RCC_APB1ENR (*(volatile uint32_t*)0x4002101Cu)
#define VTRACE_TIM2_CR1 (*(volatile uint32_t*)0x40000000u)
#define VTRACE_TIM2_DIER (*(volatile uint32_t*)0x4000000Cu)
#define VTRACE_TIM2_SR (*(volatile uint32_t*)0x40000010u)
#define VTRACE_TIM2_PSC (*(volatile uint32_t*)0x40000028u)
#define VTRACE_TIM2_ARR (*(volatile uint32_t*)0x4000002Cu)
#define VTRACE_NVIC_ISER0 (*(volatile uint32_t*)0xE000E100u)

void vtrace_rtec2_start_isr_source(void)
{
	const uint32_t timerClock = (uint32_t)configCPU_CLOCK_HZ;
	const uint32_t prescaler = (timerClock / 1000000u) > 0u ? (timerClock / 1000000u) : 1u;

	VTRACE_RCC_APB1ENR |= (1u << 0);

	VTRACE_TIM2_CR1 = 0u;
	VTRACE_TIM2_PSC = prescaler - 1u;         /* one tick per microsecond */
	VTRACE_TIM2_ARR = 20000u - 1u;            /* twenty milliseconds */
	VTRACE_TIM2_DIER = 1u;
	VTRACE_TIM2_SR = 0u;
	VTRACE_TIM2_CR1 = 1u;

	/* TIM2 is interrupt 28 on this part, and its priority goes to the lowest
	   the part has: below `configMAX_SYSCALL_INTERRUPT_PRIORITY` is the only
	   place from which the kernel's own calls may be made, and the handler
	   reads the tick. Leaving it at the reset value (highest) is what made the
	   tracer stop after a few minutes. */
	*(volatile uint8_t*)(0xE000E400u + 28u) = 0xF0u;

	VTRACE_NVIC_ISER0 = (1u << 28);
}

void vtrace_rtec2_isr_tick(void)
{
	VTRACE_TIM2_SR = 0u;

	/* Both ends of the same interrupt, one after the other: the gap between
	   them is the interrupt's own duration, which is what the ISR band is
	   drawn from. */
	rtec2_event enter = {0};

	enter.type = VTRACE_TYPE_ISR_ENTER;
	enter.hasTicks = 1;
	enter.ticks = vtrace_now_us_from_isr();

	(void)rtec2_writer_emit(&vtraceWriter, &enter);

	enter.type = VTRACE_TYPE_ISR_EXIT;
	enter.ticks = vtrace_now_us_from_isr();

	(void)rtec2_writer_emit(&vtraceWriter, &enter);
}

void vtrace_rtec2_fault(uint32_t cfsr, uint32_t pc, uint32_t lr)
{
	/* No time: the clock is read through the kernel's tick, and a fault may be
	   one of the reasons the kernel is not going to give one. The event says
	   what happened and the reader places it at the last thing it knew. */
	rtec2_event event = {0};

	event.type = VTRACE_TYPE_FAULT;
	event.subject = cfsr;
	event.arg0 = pc;
	event.arg1 = lr;

	(void)rtec2_writer_emit(&vtraceWriter, &event);
}

void vtrace_rtec2_sync(uint8_t type, uint32_t object, uint32_t task)
{
	emit(type, task, object, 0u, 0u);
}

void vtrace_rtec2_heap(uint8_t type, uint32_t address, uint32_t size)
{
	emit(type, address, 0u, size, 0u);
}
