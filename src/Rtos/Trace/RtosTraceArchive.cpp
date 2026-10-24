#include "RtosTraceArchive.hpp"

#include <cstring>

namespace
{
	/* One buffer and one cursor, in both directions. The reader refuses rather
	   than reading past the end: a short payload is a block that was written by
	   another version or cut by a crash, and a half decoded event is worse than
	   no event. */
	struct Writer
	{
		std::vector<uint8_t>& out;

		void u8v(uint8_t value)
		{
			out.push_back(value);
		}

		void u16v(uint16_t value)
		{
			out.push_back(static_cast<uint8_t>(value & 0xFF));
			out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
		}

		void u32v(uint32_t value)
		{
			for (int shift = 0; shift < 32; shift += 8)
				out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
		}

		void u64v(uint64_t value)
		{
			for (int shift = 0; shift < 64; shift += 8)
				out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
		}

		void doublev(double value)
		{
			/* Same convention as the round encoding: the number is copied, not
			   converted, because every file this program writes was written by
			   this program on a machine with the same byte order. */
			uint8_t bytes[sizeof(double)] = {};
			std::memcpy(bytes, &value, sizeof(value));
			out.insert(out.end(), bytes, bytes + sizeof(bytes));
		}

		void text(const std::string& value)
		{
			u32v(static_cast<uint32_t>(value.size()));
			out.insert(out.end(), value.begin(), value.end());
		}

		void ref(const RtosEntityRef& value)
		{
			u8v(static_cast<uint8_t>(value.kind));
			u32v(value.rawHandle);
			u32v(value.generation);
			u16v(value.legacyId);
			u8v(static_cast<uint8_t>(value.quality));
		}

		void key(const RtosEntityKey& value)
		{
			u64v(value.epoch);
			u8v(static_cast<uint8_t>(value.kind));
			u32v(value.rawHandle);
			u32v(value.generation);
		}
	};

	struct Reader
	{
		const uint8_t* at = nullptr;
		size_t size = 0;
		size_t used = 0;
		bool ok = true;

		bool need(size_t bytes)
		{
			if (!ok || used + bytes > size)
			{
				ok = false;

				return false;
			}

			return true;
		}

		uint8_t u8v()
		{
			if (!need(1))
				return 0;

			return at[used++];
		}

		uint16_t u16v()
		{
			if (!need(2))
				return 0;

			const uint16_t value = static_cast<uint16_t>(at[used] | (at[used + 1] << 8));
			used += 2;

			return value;
		}

		uint32_t u32v()
		{
			if (!need(4))
				return 0;

			uint32_t value = 0;

			for (int shift = 0; shift < 32; shift += 8)
				value |= static_cast<uint32_t>(at[used++]) << shift;

			return value;
		}

		uint64_t u64v()
		{
			if (!need(8))
				return 0;

			uint64_t value = 0;

			for (int shift = 0; shift < 64; shift += 8)
				value |= static_cast<uint64_t>(at[used++]) << shift;

			return value;
		}

		double doublev()
		{
			double value = 0.0;

			if (!need(sizeof(double)))
				return value;

			std::memcpy(&value, at + used, sizeof(value));
			used += sizeof(value);

			return value;
		}

		std::string text()
		{
			const uint32_t bytes = u32v();

			if (!need(bytes))
				return {};

			std::string value(reinterpret_cast<const char*>(at + used), bytes);
			used += bytes;

			return value;
		}

		RtosEntityRef ref()
		{
			RtosEntityRef value;

			value.kind = static_cast<RtosEntityKind>(u8v());
			value.rawHandle = u32v();
			value.generation = u32v();
			value.legacyId = u16v();
			value.quality = static_cast<RtosIdentityQuality>(u8v());

			return value;
		}

		RtosEntityKey key()
		{
			RtosEntityKey value;

			value.epoch = u64v();
			value.kind = static_cast<RtosEntityKind>(u8v());
			value.rawHandle = u32v();
			value.generation = u32v();

			return value;
		}
	};

	void writeEvent(Writer& writer, const RtosNormalizedEvent& normalized)
	{
		const RtosTraceEvent& event = normalized.event;

		writer.u64v(event.epoch);
		writer.u32v(event.sequence);
		writer.u8v(event.hasTargetTicks ? 1 : 0);
		writer.u32v(event.targetTicks);
		writer.u32v(event.targetHz);
		writer.u8v(event.coreId);
		writer.u8v(static_cast<uint8_t>(event.type));
		writer.ref(event.subject);
		writer.ref(event.object);
		writer.u32v(event.arg0);
		writer.u32v(event.arg1);
		writer.u8v(event.payloadBytes);

		for (uint8_t index = 0; index < sizeof(event.payload); index++)
			writer.u8v(index < event.payloadBytes ? event.payload[index] : 0);

		writer.u8v(static_cast<uint8_t>(event.quality));

		writer.key(normalized.subject);
		writer.key(normalized.object);

		writer.u8v(normalized.subjectGenerationUnknown ? 1 : 0);
		writer.u8v(normalized.afterGap ? 1 : 0);
		writer.u8v(normalized.hasDisplaySeconds ? 1 : 0);
		writer.doublev(normalized.displaySeconds);
	}

	RtosNormalizedEvent readEvent(Reader& reader)
	{
		RtosNormalizedEvent normalized;
		RtosTraceEvent& event = normalized.event;

		event.epoch = reader.u64v();
		event.sequence = reader.u32v();
		event.hasTargetTicks = reader.u8v() != 0;
		event.targetTicks = reader.u32v();
		event.targetHz = reader.u32v();
		event.coreId = reader.u8v();
		event.type = static_cast<RtosTraceEventType>(reader.u8v());
		event.subject = reader.ref();
		event.object = reader.ref();
		event.arg0 = reader.u32v();
		event.arg1 = reader.u32v();

		const uint8_t declared = reader.u8v();

		/* Clamped rather than trusted: the array is twelve bytes, and a payload
		   that claims more would be a read past the end of it. */
		event.payloadBytes = declared > sizeof(event.payload) ? static_cast<uint8_t>(sizeof(event.payload)) : declared;

		for (uint8_t index = 0; index < sizeof(event.payload); index++)
			event.payload[index] = reader.u8v();

		event.quality = static_cast<RtosEvidenceQuality>(reader.u8v());

		normalized.subject = reader.key();
		normalized.object = reader.key();

		normalized.subjectGenerationUnknown = reader.u8v() != 0;
		normalized.afterGap = reader.u8v() != 0;
		normalized.hasDisplaySeconds = reader.u8v() != 0;
		normalized.displaySeconds = reader.doublev();

		return normalized;
	}
}  // namespace

namespace RtosTraceArchive
{
	std::vector<uint8_t> encodeEvents(const std::vector<RtosNormalizedEvent>& events)
	{
		std::vector<uint8_t> out;
		out.reserve(32 + events.size() * 88);

		Writer writer{out};

		writer.u32v(static_cast<uint32_t>(events.size()));

		for (const RtosNormalizedEvent& event : events)
			writeEvent(writer, event);

		return out;
	}

	bool decodeEvents(const std::vector<uint8_t>& payload, std::vector<RtosNormalizedEvent>& events)
	{
		events.clear();

		Reader reader{payload.data(), payload.size(), 0, true};

		const uint32_t count = reader.u32v();

		if (!reader.ok)
			return false;

		/* A count larger than the payload could hold is refused before the loop:
		   reserving on it would be a long allocation for a corrupt number. */
		if (count > payload.size())
			return false;

		events.reserve(count);

		for (uint32_t index = 0; index < count; index++)
		{
			events.push_back(readEvent(reader));

			if (!reader.ok)
			{
				events.clear();

				return false;
			}
		}

		return true;
	}

	std::vector<uint8_t> encodeIdentities(const RtosIdentityStore& identities)
	{
		std::vector<uint8_t> out;

		const std::map<RtosEntityKey, RtosIdentityStore::Entity>& all = identities.all();

		out.reserve(16 + all.size() * 48);

		Writer writer{out};

		writer.u32v(static_cast<uint32_t>(all.size()));

		for (const auto& entry : all)
		{
			const RtosIdentityStore::Entity& entity = entry.second;

			writer.key(entity.key);
			writer.u8v(entity.nameFromDictionary ? 1 : 0);
			writer.u64v(entity.firstSeenTicks);
			writer.u8v(entity.generationUnknown ? 1 : 0);
			writer.u8v(entity.afterGap ? 1 : 0);
			writer.u8v(entity.closed ? 1 : 0);
			writer.text(entity.name);
		}

		return out;
	}

	bool decodeIdentities(const std::vector<uint8_t>& payload, RtosIdentityStore& identities)
	{
		Reader reader{payload.data(), payload.size(), 0, true};

		const uint32_t count = reader.u32v();

		if (!reader.ok || count > payload.size())
			return false;

		std::vector<RtosIdentityStore::Entity> decoded;
		decoded.reserve(count);

		/* Decoded into a local first: a payload that runs short half way must
		   leave the store as it was, not holding half of a dictionary. */
		for (uint32_t index = 0; index < count; index++)
		{
			RtosIdentityStore::Entity entity;

			entity.key = reader.key();
			entity.nameFromDictionary = reader.u8v() != 0;
			entity.firstSeenTicks = reader.u64v();
			entity.generationUnknown = reader.u8v() != 0;
			entity.afterGap = reader.u8v() != 0;
			entity.closed = reader.u8v() != 0;
			entity.name = reader.text();

			if (!reader.ok)
				return false;

			decoded.push_back(std::move(entity));
		}

		for (const RtosIdentityStore::Entity& entity : decoded)
			identities.restore(entity);

		return true;
	}

	std::vector<uint8_t> encodeSource(const RtosTraceSourceRecord& source)
	{
		std::vector<uint8_t> out;

		Writer writer{out};

		writer.text(source.name);
		writer.u32v(source.capabilities.bits);

		return out;
	}

	bool decodeSource(const std::vector<uint8_t>& payload, RtosTraceSourceRecord& source)
	{
		Reader reader{payload.data(), payload.size(), 0, true};

		source.name = reader.text();
		source.capabilities.bits = reader.u32v();

		return reader.ok;
	}
}  // namespace RtosTraceArchive
