#include "Rtec2Adapter.hpp"

bool Rtec2Adapter::beginControlBlock(const uint8_t* bytes, size_t size, RtosTraceProblem& problem)
{
	problem = RtosTraceProblem::None;

	Rtec2ControlBlock parsed;

	if (!Rtec2RingReader::parseControlBlock(bytes, size, parsed, problem))
	{
		haveBlock = false;

		return false;
	}

	/* A different epoch is a different session on the target: starting the
	   program again resets the sequence, and a cursor from before the reset
	   would read slots that mean something else now. */
	if (haveBlock && parsed.epoch != block.epoch)
	{
		cursor = 0;
		restartCount++;
	}

	block = parsed;
	haveBlock = true;

	if (sourceState == RtosTraceSourceState::NotStarted)
		sourceState = RtosTraceSourceState::Running;

	/* A cursor past the writer's head is the same event as a new epoch, seen
	   from the other side: the target went back to the beginning. */
	if (cursor > block.head)
	{
		cursor = block.head;
		restartCount++;
	}

	/* Falling behind is handled by `skipToOldestLive`, which the caller runs
	   once it has decided to read this round. Doing it here as well was how a
	   round spent its whole budget catching up and then read nothing. */

	return true;
}

uint32_t Rtec2Adapter::skipToOldestLive()
{
	if (!haveBlock)
		return 0;

	/* `head` is one past the newest record, so the ring holds
	   `head - capacity .. head - 1`, and `head - capacity` is the one the next
	   write lands on. Start one slot later or every resume reads a slot that is
	   being overwritten at that instant. */
	const uint32_t oldest = block.head - block.capacity + 1;

	if (cursor >= oldest)
		return 0;

	const uint32_t skipped = oldest - cursor;

	cursor = oldest;

	return skipped;
}

Rtec2Adapter::Window Rtec2Adapter::nextWindow(uint32_t budget) const
{
	Window window;

	windowCount++;

	if (!haveBlock || budget == 0)
		return window;

	const uint32_t available = block.head - cursor;

	if (available == 0)
		return window;

	window.from = cursor % block.capacity;
	window.slots = available < budget ? available : budget;
	window.slots = window.slots < block.capacity ? window.slots : block.capacity;

	/* Never across the end of the ring. The reader takes one contiguous run;
	   the rest is next round's work, and a caller that had to stitch two reads
	   together would be a caller that can get the stitch wrong. */
	if (window.from + window.slots > block.capacity)
		window.slots = block.capacity - window.from;

	requestedCount += window.slots;

	return window;
}

bool Rtec2Adapter::pushSlots(const uint8_t* bytes, size_t size)
{
	if (!haveBlock || bytes == nullptr || size == 0)
		return false;

	/* The buffer is the run `nextWindow` asked for, starting at the cursor -
	   not the whole ring. Reading 3.5 KB a round to collect five records was
	   most of what a slow probe spent on this, and it was why events went
	   missing: the ring wrapped before the next read got there. */
	const Rtec2RingReader::Read read = Rtec2RingReader::readWindow(bytes, size, block, cursor, static_cast<uint32_t>(size / Rtec2RingReader::slotBytes));

	/* A batch that caught a slot mid-write is abandoned whole: keeping its good
	   prefix would leave the cursor after records nobody counted, and every
	   interval computed later would cross a hole that was never reported. */
	if (read.torn != 0)
	{
		tornCount++;

		return false;
	}

	if (!read.advanced)
		return false;

	decodedCount += static_cast<uint32_t>(read.records.size());

	for (const Rtec2DecodedRecord& record : read.records)
	{
		RtosTraceEvent event = record.event;

		/* The scale is not in the record - a slot carries ticks, not the unit
		   they are in - so it comes from the control block beside them. Without
		   this every event arrives with ticks nobody can convert, and the view
		   says "no target time" for all of them while the numbers are right
		   there. */
		if (event.targetHz == 0)
			event.targetHz = block.targetHz;

		if (event.hasTargetTicks && (!sawTicks || event.targetTicks > newest))
		{
			newest = event.targetTicks;
			sawTicks = true;
		}

		pending.push_back(event);
	}

	cursor = read.committed;

	if (read.restarted)
		restartCount++;

	return true;
}

bool Rtec2Adapter::start(RtosTraceProblem& problem)
{
	problem = RtosTraceProblem::None;

	/* Nothing to open: the caller does the reading. A source that reported a
	   failure here would be blaming itself for a probe it does not touch. */
	sourceState = RtosTraceSourceState::Running;

	return true;
}

void Rtec2Adapter::stop()
{
	sourceState = RtosTraceSourceState::Stopped;

	pending.clear();
	haveBlock = false;
	cursor = 0;
}

void Rtec2Adapter::requestNewEpoch()
{
	/* The epoch belongs to the target. Asking for a new one means forgetting
	   the cursor and letting the next control block read say what the target is
	   doing now. */
	cursor = 0;
}

RtosTraceCapabilities Rtec2Adapter::capabilities() const
{
	RtosTraceCapabilities capabilities;

	/* The control block carries the writer's own answer to "what do you send",
	   which is the only answer worth showing: the panel turns every bit that is
	   not set into "this source does not carry it". */
	capabilities.bits = haveBlock ? block.capabilities : 0;

	return capabilities;
}

RtosTraceSourceState Rtec2Adapter::state() const
{
	return sourceState;
}

RtosTraceBatch Rtec2Adapter::poll(double budgetSeconds)
{
	(void)budgetSeconds;

	RtosTraceBatch batch;

	batch.cursorBefore = cursor;
	batch.torn = false;
	batch.readFailed = false;

	while (!pending.empty())
	{
		batch.events.push_back(pending.front());
		pending.pop_front();
	}

	batch.cursorAfter = cursor;
	batch.state = sourceState;
	batch.capabilities = capabilities();

	return batch;
}
