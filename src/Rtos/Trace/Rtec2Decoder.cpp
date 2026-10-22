#include "Rtec2Decoder.hpp"

#include <cstring>

namespace
{
	constexpr size_t headerBytes = 24;
	constexpr size_t maxPayload = 12;

	uint32_t readU32(const uint8_t* bytes)
	{
		return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) |
		       (static_cast<uint32_t>(bytes[3]) << 24);
	}
}  // namespace

RtosTraceEventType Rtec2Decoder::typeOf(uint8_t value)
{
	/* The wire numbers are the enum's numbers by construction; the switch is
	   here so that a value in a reserved range comes back as Unknown rather
	   than as a cast that happens to compile. */
	switch (value)
	{
		case 0x01:
			return RtosTraceEventType::EpochBegin;
		case 0x02:
			return RtosTraceEventType::ClockSync;
		case 0x03:
			return RtosTraceEventType::Gap;
		case 0x04:
			return RtosTraceEventType::SourceStatus;
		case 0x10:
			return RtosTraceEventType::TaskCreate;
		case 0x11:
			return RtosTraceEventType::TaskDelete;
		case 0x12:
			return RtosTraceEventType::TaskRename;
		case 0x13:
			return RtosTraceEventType::TaskSwitchIn;
		case 0x14:
			return RtosTraceEventType::TaskSwitchOut;
		case 0x15:
			return RtosTraceEventType::TaskReady;
		case 0x16:
			return RtosTraceEventType::TaskBlock;
		case 0x17:
			return RtosTraceEventType::TaskWake;
		case 0x18:
			return RtosTraceEventType::PriorityChange;
		case 0x20:
			return RtosTraceEventType::SyncCreate;
		case 0x21:
			return RtosTraceEventType::SyncDelete;
		case 0x22:
			return RtosTraceEventType::WaitBegin;
		case 0x23:
			return RtosTraceEventType::WaitEnd;
		case 0x24:
			return RtosTraceEventType::Signal;
		case 0x25:
			return RtosTraceEventType::Acquire;
		case 0x26:
			return RtosTraceEventType::Release;
		case 0x30:
			return RtosTraceEventType::IsrEnter;
		case 0x31:
			return RtosTraceEventType::IsrExit;
		case 0x40:
			return RtosTraceEventType::Fault;
		case 0x50:
			return RtosTraceEventType::Alloc;
		case 0x51:
			return RtosTraceEventType::Free;
		default:
			return RtosTraceEventType::Unknown;
	}
}

RtosEntityKind Rtec2Decoder::subjectKind(RtosTraceEventType type)
{
	switch (type)
	{
		case RtosTraceEventType::TaskCreate:
		case RtosTraceEventType::TaskDelete:
		case RtosTraceEventType::TaskRename:
		case RtosTraceEventType::TaskSwitchIn:
		case RtosTraceEventType::TaskSwitchOut:
		case RtosTraceEventType::TaskReady:
		case RtosTraceEventType::TaskBlock:
		case RtosTraceEventType::TaskWake:
		case RtosTraceEventType::PriorityChange:
		case RtosTraceEventType::WaitBegin:
		case RtosTraceEventType::WaitEnd:
		case RtosTraceEventType::Signal:
		case RtosTraceEventType::Acquire:
		case RtosTraceEventType::Release:
			return RtosEntityKind::Task;

		default:
			/* A gap, a fault, an allocation: they are about the session or the
			   machine, not about one task. `arg0` carries the IRQ number for an
			   ISR, which is not a handle and must not be keyed like one. */
			return RtosEntityKind::None;
	}
}

RtosEntityKind Rtec2Decoder::objectKind(RtosTraceEventType type)
{
	switch (type)
	{
		case RtosTraceEventType::SyncCreate:
		case RtosTraceEventType::SyncDelete:
		case RtosTraceEventType::WaitBegin:
		case RtosTraceEventType::WaitEnd:
		case RtosTraceEventType::Signal:
		case RtosTraceEventType::Acquire:
		case RtosTraceEventType::Release:
		case RtosTraceEventType::TaskBlock:
		case RtosTraceEventType::TaskWake:
			return RtosEntityKind::Sync;

		default:
			return RtosEntityKind::None;
	}
}

size_t Rtec2Decoder::recordBytes(const uint8_t* bytes, size_t size)
{
	if (bytes == nullptr || size < headerBytes)
		return 0;

	const uint8_t flags = bytes[1];
	size_t bytesUsed = headerBytes;

	if ((flags & 0x01u) != 0)
	{
		if (size < bytesUsed + 4)
			return 0;

		bytesUsed += 4;
	}

	if ((flags & 0x02u) != 0)
	{
		if (size < bytesUsed + 1)
			return 0;

		const uint8_t payloadBytes = bytes[bytesUsed];

		if (payloadBytes > maxPayload || size < bytesUsed + 1 + payloadBytes)
			return 0;

		bytesUsed += 1 + payloadBytes;
	}

	return bytesUsed;
}

Rtec2DecodeResult Rtec2Decoder::decode(const uint8_t* bytes, size_t size)
{
	Rtec2DecodeResult result;

	if (bytes == nullptr)
		return result;

	size_t at = 0;

	while (at < size)
	{
		const size_t used = recordBytes(bytes + at, size - at);

		/* A record that does not fit: everything from here on is the tail of
		   something that was cut off. Reported, not guessed at. */
		if (used == 0)
		{
			result.truncatedBytes = size - at;
			break;
		}

		const uint8_t* record = bytes + at;

		Rtec2DecodedRecord decoded;
		decoded.event.type = typeOf(record[0]);
		decoded.event.sequence = readU32(record + 4);

		const uint32_t subject = readU32(record + 8);
		const uint32_t object = readU32(record + 12);

		decoded.event.subject.kind = subjectKind(decoded.event.type);
		decoded.event.subject.rawHandle = subject;
		decoded.event.subject.quality = subject == 0 ? RtosIdentityQuality::Unknown : RtosIdentityQuality::Exact;

		decoded.event.object.kind = objectKind(decoded.event.type);
		decoded.event.object.rawHandle = object;
		decoded.event.object.quality = object == 0 ? RtosIdentityQuality::Unknown : RtosIdentityQuality::Exact;

		decoded.event.arg0 = readU32(record + 16);
		decoded.event.arg1 = readU32(record + 20);

		size_t cursor = headerBytes;

		if ((record[1] & 0x01u) != 0)
		{
			decoded.event.hasTargetTicks = true;
			decoded.event.targetTicks = readU32(record + cursor);
			cursor += 4;
		}

		if ((record[1] & 0x02u) != 0)
		{
			decoded.payloadBytes = record[cursor];
			std::memcpy(decoded.payload, record + cursor + 1, decoded.payloadBytes);
		}

		if (decoded.event.type == RtosTraceEventType::Unknown)
			result.unknownTypes++;
		else
			result.records.push_back(decoded);

		at += used;
	}

	return result;
}
