#include "RtosTraceReducer.hpp"

#include <map>

namespace
{
	uint16_t readU16(const uint8_t* at)
	{
		return static_cast<uint16_t>(at[0] | (at[1] << 8));
	}

	uint32_t readU32(const uint8_t* at)
	{
		return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) | (static_cast<uint32_t>(at[2]) << 16) |
		       (static_cast<uint32_t>(at[3]) << 24);
	}

	/* The fault frame, wire format §3.4. Every field is reported only if its
	   flag says it was there: a zero PC is a plausible address, and calling it
	   one because the field was missing is the kind of quiet wrong answer this
	   whole layer exists to avoid. */
	void readFault(const RtosTraceEvent& event, RtosTrackEntry& entry)
	{
		if (event.payloadBytes >= 4)
		{
			entry.faultClass = event.payload[0];
			entry.faultFlags = event.payload[1];
			entry.cfsr = readU16(event.payload + 2);
		}

		if ((entry.faultFlags & 0x02u) != 0 && event.payloadBytes >= 8)
			entry.pc = readU32(event.payload + 4);

		if ((entry.faultFlags & 0x04u) != 0 && event.payloadBytes >= 12)
			entry.lr = readU32(event.payload + 8);
	}

	RtosTrackEntry entryFrom(const RtosNormalizedEvent& normalized, size_t index)
	{
		RtosTrackEntry entry;

		entry.firstEvent = index;
		entry.lastEvent = index;
		entry.hasSeconds = normalized.hasDisplaySeconds;
		entry.fromSeconds = normalized.displaySeconds;
		entry.toSeconds = normalized.displaySeconds;

		return entry;
	}
}  // namespace

RtosTrackProjection RtosTraceReducer::project(const std::vector<RtosNormalizedEvent>& events)
{
	RtosTrackProjection projection;

	/* One nesting stack for interrupts: an ISR can be entered while another is
	   running, and the inner one's interval sits inside the outer one's. A
	   single "current ISR" variable would lose the outer one. */
	struct OpenIsr
	{
		size_t eventIndex = 0;
		bool hasSeconds = false;
		double seconds = 0.0;
		uint32_t irq = 0;
		uint32_t depth = 0;
	};

	std::vector<OpenIsr> isrStack;

	/* Waits, keyed by the task and the object: one task can wait for two
	   objects over a session, and two tasks can wait for the same one. */
	std::map<std::pair<uint32_t, uint32_t>, std::pair<size_t, size_t>> openWaits;  /* -> (eventIndex, subject handle) */

	for (size_t index = 0; index < events.size(); index++)
	{
		const RtosNormalizedEvent& normalized = events[index];
		const RtosTraceEvent& event = normalized.event;

		switch (event.type)
		{
			case RtosTraceEventType::IsrEnter:
			{
				OpenIsr open;
				open.eventIndex = index;
				open.hasSeconds = normalized.hasDisplaySeconds;
				open.seconds = normalized.displaySeconds;
				open.irq = event.arg0;
				open.depth = event.arg1;

				isrStack.push_back(open);
				break;
			}

			case RtosTraceEventType::IsrExit:
			{
				if (isrStack.empty())
				{
					projection.orphanIsr++;
					break;
				}

				const OpenIsr open = isrStack.back();
				isrStack.pop_back();

				RtosTrackEntry entry;
				entry.kind = RtosTrackEntry::Kind::Isr;
				entry.firstEvent = open.eventIndex;
				entry.lastEvent = index;
				entry.address = open.irq;
				entry.depth = open.depth;
				entry.name = "irq " + std::to_string(open.irq);

				/* A duration needs both ends to carry a time. One end without is
				   not half a duration, it is no duration. */
				entry.hasSeconds = open.hasSeconds && normalized.hasDisplaySeconds;
				entry.fromSeconds = open.seconds;
				entry.toSeconds = normalized.displaySeconds;

				projection.isr.push_back(entry);
				break;
			}

			case RtosTraceEventType::Fault:
			{
				RtosTrackEntry entry = entryFrom(normalized, index);
				entry.kind = RtosTrackEntry::Kind::Fault;
				readFault(event, entry);

				projection.fault.push_back(entry);
				break;
			}

			case RtosTraceEventType::WaitBegin:
			{
				const std::pair<uint32_t, uint32_t> key(event.subject.rawHandle, event.object.rawHandle);

				openWaits[key] = std::make_pair(index, static_cast<size_t>(event.subject.rawHandle));
				break;
			}

			case RtosTraceEventType::WaitEnd:
			{
				const std::pair<uint32_t, uint32_t> key(event.subject.rawHandle, event.object.rawHandle);
				const auto found = openWaits.find(key);

				if (found == openWaits.end())
				{
					projection.orphanSync++;
					break;
				}

				const size_t beganAt = found->second.first;
				openWaits.erase(found);

				RtosTrackEntry entry;
				entry.kind = RtosTrackEntry::Kind::Sync;
				entry.firstEvent = beganAt;
				entry.lastEvent = index;
				entry.address = event.object.rawHandle;
				entry.waitResult = static_cast<uint8_t>(event.arg0);
				entry.name = normalized.object.rawHandle != 0 ? std::to_string(normalized.object.rawHandle) : std::string();

				entry.hasSeconds = normalized.hasDisplaySeconds && events[beganAt].hasDisplaySeconds;
				entry.fromSeconds = events[beganAt].displaySeconds;
				entry.toSeconds = normalized.displaySeconds;

				projection.sync.push_back(entry);
				break;
			}

			case RtosTraceEventType::Alloc:
			case RtosTraceEventType::Free:
			{
				RtosTrackEntry entry = entryFrom(normalized, index);
				entry.kind = RtosTrackEntry::Kind::Heap;
				entry.address = event.arg0;
				entry.bytes = event.arg1;
				entry.allocated = event.type == RtosTraceEventType::Alloc;

				/* An unknown size is not zero bytes: it is a fact that is missing,
				   and adding zero for it would make the net look smaller than it
				   is. */
				if (entry.bytes == 0)
					projection.heapUnknownSize++;
				else
					projection.heapNetBytes += entry.allocated ? static_cast<int64_t>(entry.bytes) : -static_cast<int64_t>(entry.bytes);

				projection.heap.push_back(entry);
				break;
			}

			case RtosTraceEventType::TaskSwitchIn:
			{
				RtosTrackEntry entry = entryFrom(normalized, index);
				entry.kind = RtosTrackEntry::Kind::Task;
				entry.address = event.subject.rawHandle;

				projection.task.push_back(entry);
				break;
			}

			case RtosTraceEventType::Gap:
			{
				RtosTrackEntry entry = entryFrom(normalized, index);
				entry.kind = RtosTrackEntry::Kind::Gap;
				entry.lostRecords = event.arg0;
				entry.gapReason = static_cast<uint8_t>(event.arg1);

				projection.gap.push_back(entry);
				break;
			}

			default:
				break;
		}
	}

	/* Whatever is still on the stack never got its second event. Counted, not
	   drawn: an interval of unknown length is not a shorter interval. */
	projection.unclosedIsr = static_cast<uint32_t>(isrStack.size());
	projection.unclosedSync = static_cast<uint32_t>(openWaits.size());

	return projection;
}
