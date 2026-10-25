#include "Rtec2SwoSource.hpp"

#include <algorithm>

namespace
{
	/* Which capability bits a record implies. Used until the target says
	   otherwise, and only for the four a stream can demonstrate by arriving:
	   everything else is left clear, because a bit set without evidence shows
	   an empty track as "the target had nothing" instead of "this carrier does
	   not say". */
	void noteCapability(RtosTraceCapabilities& capabilities, RtosTraceEventType type)
	{
		switch (type)
		{
			case RtosTraceEventType::TaskCreate:
			case RtosTraceEventType::TaskDelete:
				capabilities.set(1, true); /* lifecycle */
				break;

			case RtosTraceEventType::IsrEnter:
			case RtosTraceEventType::IsrExit:
				capabilities.set(6, true); /* isrEvents */
				break;

			case RtosTraceEventType::Fault:
				capabilities.set(7, true); /* faultEvents */
				break;

			case RtosTraceEventType::SyncCreate:
			case RtosTraceEventType::SyncDelete:
			case RtosTraceEventType::WaitBegin:
			case RtosTraceEventType::WaitEnd:
			case RtosTraceEventType::Signal:
			case RtosTraceEventType::Acquire:
			case RtosTraceEventType::Release:
				capabilities.set(8, true); /* syncEvents */
				break;

			case RtosTraceEventType::Alloc:
			case RtosTraceEventType::Free:
				capabilities.set(9, true); /* heapEvents */
				break;

			default:
				break;
		}
	}
}  // namespace

void Rtec2SwoSource::feed(const uint8_t* bytes, size_t size)
{
	if (sourceState != RtosTraceSourceState::Running)
		return;

	frames.feed(bytes, size);

	for (Rtec2Frame& frame : frames.take())
	{
		counters.frames++;

		const Rtec2DecodeResult decoded = Rtec2Decoder::decode(frame.payload);

		counters.records += static_cast<uint32_t>(decoded.records.size());
		counters.unknownTypes += decoded.unknownTypes;

		for (const Rtec2DecodedRecord& record : decoded.records)
			pending.push_back(record);
	}

	counters.badFrames = frames.stats().crcFailures + frames.stats().badEnd + frames.stats().badLength + frames.stats().badPayload;
	counters.resyncBytes = frames.stats().resyncBytes;
}

bool Rtec2SwoSource::start(RtosTraceProblem& problem)
{
	problem = RtosTraceProblem::None;

	sourceState = RtosTraceSourceState::Running;

	return true;
}

void Rtec2SwoSource::stop()
{
	sourceState = RtosTraceSourceState::Stopped;

	pending.clear();
}

void Rtec2SwoSource::requestNewEpoch()
{
	newEpochWanted = true;
}

RtosTraceCapabilities Rtec2SwoSource::capabilities() const
{
	return caps;
}

RtosTraceSourceState Rtec2SwoSource::state() const
{
	return sourceState;
}

RtosTraceBatch Rtec2SwoSource::poll(double budgetSeconds)
{
	(void)budgetSeconds;

	if (newEpochWanted)
	{
		epoch++;
		newEpochWanted = false;
	}

	RtosTraceBatch batch;

	batch.cursorBefore = cursor;
	batch.cursorAfter = cursor;
	batch.state = sourceState;
	batch.capabilities = caps;

	while (!pending.empty())
	{
		const Rtec2DecodedRecord record = pending.front();

		pending.pop_front();

		RtosTraceEvent event = record.event;

		event.epoch = epoch;

		/* The stream says what the ticks are in, once a second and nowhere
		   else. A ClockSync carries the number in arg0; every other record is
		   stamped with the last one heard, because a record that arrives with
		   no unit turns into a time nobody can compute and the view says "no
		   target time" about numbers that are perfectly good. */
		if (event.type == RtosTraceEventType::ClockSync && event.arg0 != 0)
		{
			counters.targetHz = event.arg0;

			event.targetHz = event.arg0;
		}
		else if (event.targetHz == 0)
		{
			event.targetHz = counters.targetHz;
		}

		event.payloadBytes = record.payloadBytes;

		for (uint8_t index = 0; index < record.payloadBytes && index < sizeof(event.payload); index++)
			event.payload[index] = record.payload[index];

		/* An epoch begin says nothing about which entities exist, and treating
		   it as a switch would join the two run before it. */
		if (event.type == RtosTraceEventType::EpochBegin && event.payloadBytes >= 4)
		{
			const uint32_t bitmap = static_cast<uint32_t>(event.payload[0]) | (static_cast<uint32_t>(event.payload[1]) << 8) |
			                        (static_cast<uint32_t>(event.payload[2]) << 16) | (static_cast<uint32_t>(event.payload[3]) << 24);

			if (bitmap != 0)
				caps.bits = bitmap;
		}

		noteCapability(caps, event.type);

		/* A record that carries a handle is a handle of that namespace, and a
		   stream is the only carrier where nothing else says which one it is. */
		event.subject.kind = Rtec2Decoder::subjectKind(event.type);
		event.object.kind = Rtec2Decoder::objectKind(event.type);

		event.sequence = static_cast<uint32_t>(cursor + 1);

		batch.events.push_back(event);
		cursor++;
	}

	batch.cursorAfter = cursor;
	batch.capabilities = caps;
	batch.unknownRecords = counters.unknownTypes;

	return batch;
}
