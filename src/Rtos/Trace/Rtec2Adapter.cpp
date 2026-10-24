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

	if (cursor < block.head - block.capacity + 1)
	{
		/* Fell more than a ring behind: start at the oldest record that is
		   whole, not at the slot boundary the ring lays out from.
		 *
		 * The writer publishes `head = sequence + 1`, so `head - capacity` is
		 * still a live record - but it is the one the next write lands on, and
		 * the write starts before it updates head. A reader resumed onto it
		 * therefore reads a slot being overwritten at that instant, the
		 * sentinels call the batch torn, the cursor does not move, and the next
		 * round starts on the same doomed slot. The result is a reader that
		 * falls behind once and then shows a fiftieth of what the target
		 * writes, which is exactly what this looked like. */
		cursor = block.head - block.capacity + 1;
	}

	return true;
}

Rtec2Adapter::Window Rtec2Adapter::nextWindow(uint32_t budget) const
{
	Window window;

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

	return window;
}

void Rtec2Adapter::pushSlots(const uint8_t* bytes, size_t size)
{
	if (!haveBlock || bytes == nullptr || size == 0)
		return;

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

		return;
	}

	if (!read.advanced)
		return;

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

		pending.push_back(event);
	}

	cursor = read.committed;

	if (read.restarted)
		restartCount++;
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
