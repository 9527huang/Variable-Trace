#ifndef _RTEC2ADAPTER_HPP
#define _RTEC2ADAPTER_HPP

#include <cstdint>
#include <deque>
#include <vector>

#include "IRtosTraceSource.hpp"
#include "Rtec2RingReader.hpp"

/*
 * The target's RTEC2 ring, turned into events.
 *
 * Read the same way the old eight-byte ring was: the acquisition thread owns
 * the probe, so it does the reads and hands the bytes over. That is not a
 * convenience - a second reader on a J-Link is a second connection, and the
 * one that loses is whichever asked second.
 *
 * The split is therefore:
 *
 *   the caller   reads 64 bytes of control block once per round,
 *                asks `nextWindow()` how many slots to read and where from,
 *                reads that many slots, and hands them to `pushSlots`.
 *
 *   this class   parses the control block, keeps the cursor, decodes whole
 *                records, turns them into events, and refuses a batch that
 *                caught a slot mid-write rather than keeping its good prefix.
 *
 * The cursor only moves when a batch decodes cleanly, which is what makes the
 * refusal safe: the next round asks for the same slots again.
 */
class Rtec2Adapter final : public IRtosTraceSource
{
   public:
	struct Window
	{
		/* Which slot to start at, and how many to read. Zero slots means the
		   reader is level with the writer. */
		uint32_t from = 0;
		uint32_t slots = 0;
	};

	Rtec2Adapter() = default;

	/* Parses and remembers the control block. False when the bytes do not
	   describe a ring, which is what a wrong address looks like. */
	bool beginControlBlock(const uint8_t* bytes, size_t size, RtosTraceProblem& problem);

	bool hasControlBlock() const
	{
		return haveBlock;
	}

	/* Slots to read this round, capped by `budget` slots and by the ring's own
	   capacity - reading more than it holds would read past it. */
	Window nextWindow(uint32_t budget) const;

	/* Hands over exactly the slots `nextWindow` asked for. A batch that does
	   not decode cleanly is dropped whole and the cursor stays put. */
	void pushSlots(const uint8_t* bytes, size_t size);

	bool start(RtosTraceProblem& problem) override;
	void stop() override;
	void requestNewEpoch() override;

	RtosTraceCapabilities capabilities() const override;
	RtosTraceSourceState state() const override;

	const char* name() const override
	{
		return "rtec2";
	}

	RtosTraceBatch poll(double budgetSeconds) override;

	/* The ring's own numbers, for the panel to show beside the source name:
	   how many records the writer had to overwrite, and whether it has started
	   a new epoch. */
	/* How many slots the whole ring is. The reader wants all of them in one
	   buffer - it indexes by `sequence & (capacity - 1)` - so the caller reads
	   the whole ring and this is how big that is. */
	uint32_t capacity() const
	{
		return haveBlock ? block.capacity : 0;
	}

	uint32_t overwritten() const
	{
		return block.overwritten;
	}

	uint32_t cursorNow() const
	{
		return cursor;
	}

	uint32_t writerHead() const
	{
		return haveBlock ? block.head : 0;
	}

	uint32_t tornBatches() const
	{
		return tornCount;
	}

	uint32_t unknownTypes() const
	{
		return unknownTypeCount;
	}

   private:
	Rtec2ControlBlock block;
	bool haveBlock = false;

	/* Where the next slot comes from. Counts records, like the writer's head. */
	uint32_t cursor = 0;

	std::deque<RtosTraceEvent> pending;

	uint32_t tornCount = 0;
	uint32_t unknownTypeCount = 0;
	uint32_t restartCount = 0;

	RtosTraceSourceState sourceState = RtosTraceSourceState::NotStarted;
};

#endif
