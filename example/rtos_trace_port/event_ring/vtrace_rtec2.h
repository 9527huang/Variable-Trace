/*
 * The event ring, in the RTEC2 container this time.
 *
 * The old path wrote eight bytes per record - a TCB address and a time - which
 * is exactly enough for a task switch and nothing else. An interrupt, a fault,
 * a queue send and a malloc all need a type, and some need a payload, so the
 * question "which kind of thing happened" had nowhere to be answered.
 *
 * RTEC2 answers it: 56-byte slots with a type byte, a subject, two arguments
 * and up to twelve bytes of payload, written under two sentinels so a reader
 * never takes half a record. `example/rtec2_port/` holds the writer; this file
 * is the firmware's use of it - one symbol the host can find, and one function
 * per hook the kernel offers.
 *
 * Capacity is 64 slots on the F103, which sounds small until the other number
 * is put beside it: the host reads the ring on every sampling round, ten
 * milliseconds apart, and the busiest target here switches about three hundred
 * times a second. Three records per round against sixty-four slots is not a
 * margin, it is a rounding error away from one.
 */

#ifndef VTRACE_RTEC2_H
#define VTRACE_RTEC2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Where the ring lives. The host finds this symbol in the symbol file and reads
   the control block at its first byte, so it must stay one flat array: a struct
   whose members the host has to follow a pointer to reach would be a struct the
   host cannot read. */
#define VTRACE_RTEC2_CAPACITY 64u
extern uint8_t vtrace_rtec2_blob[];

/* Prepares the ring. Called once, before the scheduler starts. */
void vtrace_rtec2_init(void);

/*
 * The hooks. All of them are safe from an interrupt, take no locks, allocate
 * nothing and return nothing: an event that cannot be written is counted and
 * dropped, because a tracer that stalls the scheduler measures something else.
 */

void vtrace_rtec2_switch(uint32_t tcb);

void vtrace_rtec2_isr_enter(void);
void vtrace_rtec2_isr_exit(void);

/* Starts the timer that gives the ISR track something to draw: one interrupt
   every twenty milliseconds, each one recording its own entry and exit. */
void vtrace_rtec2_start_isr_source(void);

/* The body of that timer's handler. Called from TIM2_IRQHandler. */
void vtrace_rtec2_isr_tick(void);

/* CFSR, the stacked PC and the stacked LR, so that a crash arrives with the
   three numbers somebody will ask for first. */
void vtrace_rtec2_fault(uint32_t cfsr, uint32_t pc, uint32_t lr);

/* `type` is a wire type: 0x22 wait begin, 0x23 wait end, 0x24 signal. */
void vtrace_rtec2_sync(uint8_t type, uint32_t object, uint32_t task);

/* `type` is a wire type: 0x50 alloc, 0x51 free. */
void vtrace_rtec2_heap(uint8_t type, uint32_t address, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
