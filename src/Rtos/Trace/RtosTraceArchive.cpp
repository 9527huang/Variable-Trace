#include "RtosTraceArchive.hpp"

#include <zstd.h>

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

		void float32v(float value)
		{
			uint8_t bytes[sizeof(float)] = {};
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

		float float32v()
		{
			float value = 0.0f;

			if (!need(sizeof(float)))
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

	/* The version 1 layout, kept so that an older recording still reads. */
	void writeEventV1(Writer& writer, const RtosNormalizedEvent& normalized)
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

	RtosNormalizedEvent readEventV1(Reader& reader)
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
	/* Version 2: what repeats is written once.
	 *
	 * Per event: type, flags, sequence, two arguments, one handle per side with
	 * the generation that side needs, the clock and the display second. What is
	 * left out is everything the reader can put back: an epoch shared by the
	 * whole block, the payload padding, and the handle's kind and value written
	 * a second time as part of an identity key. */
	void writeEventV2(Writer& writer, const RtosNormalizedEvent& normalized, uint64_t blockEpoch)
	{
		const RtosTraceEvent& event = normalized.event;

		uint8_t flags = 0;
		flags |= event.hasTargetTicks ? 0x01 : 0;
		flags |= normalized.hasDisplaySeconds ? 0x02 : 0;
		flags |= normalized.subject.generation != event.subject.generation ? 0x04 : 0;
		flags |= normalized.object.generation != event.object.generation ? 0x08 : 0;
		flags |= normalized.subjectGenerationUnknown ? 0x10 : 0;
		flags |= normalized.afterGap ? 0x20 : 0;
		flags |= event.payloadBytes > 0 ? 0x40 : 0;

		writer.u8v(static_cast<uint8_t>(event.type));
		writer.u8v(flags);
		writer.u8v(event.coreId);
		writer.u32v(event.sequence);
		writer.u32v(event.arg0);
		writer.u32v(event.arg1);

		/* One handle per side: kind and value from the identity key, which is
		   what the reference carries too; the generation twice only when the
		   store decided a different one. */
		writer.u8v(static_cast<uint8_t>(normalized.subject.kind));
		writer.u32v(normalized.subject.rawHandle);
		writer.u32v(normalized.subject.generation);
		if ((flags & 0x04) != 0)
			writer.u32v(event.subject.generation);
		writer.u8v(static_cast<uint8_t>(event.subject.quality));
		writer.u16v(event.subject.legacyId);

		writer.u8v(static_cast<uint8_t>(normalized.object.kind));
		writer.u32v(normalized.object.rawHandle);
		writer.u32v(normalized.object.generation);
		if ((flags & 0x08) != 0)
			writer.u32v(event.object.generation);
		writer.u8v(static_cast<uint8_t>(event.object.quality));
		writer.u16v(event.object.legacyId);

		if (event.hasTargetTicks)
			writer.u32v(event.targetTicks);

		writer.u32v(event.targetHz);
		writer.u8v(static_cast<uint8_t>(event.quality));

		if (event.payloadBytes > 0)
		{
			const uint8_t bytes = event.payloadBytes > sizeof(event.payload) ? static_cast<uint8_t>(sizeof(event.payload)) : event.payloadBytes;

			writer.u8v(bytes);

			for (uint8_t index = 0; index < bytes; index++)
				writer.u8v(event.payload[index]);
		}

		if (normalized.hasDisplaySeconds)
			writer.float32v(static_cast<float>(normalized.displaySeconds));
	}

	RtosNormalizedEvent readEventV2(Reader& reader, uint64_t blockEpoch)
	{
		RtosNormalizedEvent normalized;
		RtosTraceEvent& event = normalized.event;

		event.type = static_cast<RtosTraceEventType>(reader.u8v());

		const uint8_t flags = reader.u8v();

		event.coreId = reader.u8v();
		event.sequence = reader.u32v();
		event.arg0 = reader.u32v();
		event.arg1 = reader.u32v();

		normalized.subject.kind = static_cast<RtosEntityKind>(reader.u8v());
		normalized.subject.rawHandle = reader.u32v();
		normalized.subject.generation = reader.u32v();

		event.subject.kind = normalized.subject.kind;
		event.subject.rawHandle = normalized.subject.rawHandle;
		event.subject.generation = (flags & 0x04) != 0 ? reader.u32v() : normalized.subject.generation;
		event.subject.quality = static_cast<RtosIdentityQuality>(reader.u8v());
		event.subject.legacyId = reader.u16v();

		normalized.object.kind = static_cast<RtosEntityKind>(reader.u8v());
		normalized.object.rawHandle = reader.u32v();
		normalized.object.generation = reader.u32v();

		event.object.kind = normalized.object.kind;
		event.object.rawHandle = normalized.object.rawHandle;
		event.object.generation = (flags & 0x08) != 0 ? reader.u32v() : normalized.object.generation;
		event.object.quality = static_cast<RtosIdentityQuality>(reader.u8v());
		event.object.legacyId = reader.u16v();

		event.hasTargetTicks = (flags & 0x01) != 0;

		if (event.hasTargetTicks)
			event.targetTicks = reader.u32v();

		event.targetHz = reader.u32v();
		event.quality = static_cast<RtosEvidenceQuality>(reader.u8v());

		if ((flags & 0x40) != 0)
		{
			const uint8_t bytes = reader.u8v();

			event.payloadBytes = bytes > sizeof(event.payload) ? static_cast<uint8_t>(sizeof(event.payload)) : bytes;

			for (uint8_t index = 0; index < bytes; index++)
			{
				const uint8_t value = reader.u8v();

				if (index < sizeof(event.payload))
					event.payload[index] = value;
			}
		}

		normalized.subjectGenerationUnknown = (flags & 0x10) != 0;
		normalized.afterGap = (flags & 0x20) != 0;
		normalized.hasDisplaySeconds = (flags & 0x02) != 0;

		if (normalized.hasDisplaySeconds)
			normalized.displaySeconds = reader.float32v();

		/* The epoch is the block's: a file that holds two epochs in one block is
		   a file this writer never produced. The keys carry it too - an identity
		   is (epoch, kind, handle, generation), and a key without its epoch
		   would match an entity from another session. */
		event.epoch = blockEpoch;
		normalized.subject.epoch = blockEpoch;
		normalized.object.epoch = blockEpoch;

		return normalized;
	}
}  // namespace

namespace RtosTraceArchive
{
	std::vector<uint8_t> encodeEvents(const std::vector<RtosNormalizedEvent>& events)
	{
		std::vector<uint8_t> out;

		out.reserve(16 + events.size() * 40);

		Writer writer{out};

		/* The epoch is a session wide number: written once when the whole block
		   shares one, which is every block this writer produces. A block that
		   does not gets one per event, so the reader never has to guess. */
		uint64_t sharedEpoch = events.empty() ? 0 : events.front().event.epoch;
		bool oneEpoch = true;

		for (const RtosNormalizedEvent& event : events)
		{
			if (event.event.epoch != sharedEpoch)
			{
				oneEpoch = false;

				break;
			}
		}

		writer.u8v(oneEpoch ? 1 : 0);

		if (oneEpoch)
			writer.u64v(sharedEpoch);

		writer.u32v(static_cast<uint32_t>(events.size()));

		for (const RtosNormalizedEvent& event : events)
		{
			if (!oneEpoch)
				writer.u64v(event.event.epoch);

			writeEventV2(writer, event, sharedEpoch);
		}

		/* Compressed on the way out. A block is a few kilobytes of records that
		   differ in two fields; the frame header costs twelve bytes and is worth
		   it from the first hundred events, which is one round. */
		std::vector<uint8_t> compressed;

		compressed.resize(ZSTD_compressBound(out.size()));

		const size_t written = ZSTD_compress(compressed.data(), compressed.size(), out.data(), out.size(), 3);

		if (ZSTD_isError(written) != 0)
			return out; /* uncompressed is still correct, and this is the honest fallback */

		compressed.resize(written);

		return compressed;
	}

	bool decodeEvents(const std::vector<uint8_t>& payload, std::vector<RtosNormalizedEvent>& events, uint16_t version)
	{
		events.clear();

		if (version >= 3)
		{
			const unsigned long long size = ZSTD_getFrameContentSize(payload.data(), payload.size());

			/* A frame without its size, or one that claims more than a block can
			   hold, is not this writer's: refuse rather than allocate on a
			   number a corrupt file chose. */
			if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > (16u << 20))
				return false;

			std::vector<uint8_t> raw(static_cast<size_t>(size));

			const size_t got = ZSTD_decompress(raw.data(), raw.size(), payload.data(), payload.size());

			if (ZSTD_isError(got) != 0 || got != raw.size())
				return false;

			return decodeEvents(raw, events, 2);
		}

		Reader reader{payload.data(), payload.size(), 0, true};

		/* Version 1 wrote the count first and every field of every event; a file
		   from before this change is somebody's evidence, so it still reads. */
		if (version <= 1)
		{
			const uint32_t count = reader.u32v();

			if (!reader.ok || count > payload.size())
				return false;

			events.reserve(count);

			for (uint32_t index = 0; index < count; index++)
			{
				events.push_back(readEventV1(reader));

				if (!reader.ok)
				{
					events.clear();

					return false;
				}
			}

			return true;
		}

		const bool oneEpoch = reader.u8v() != 0;
		const uint64_t sharedEpoch = oneEpoch ? reader.u64v() : 0;
		const uint32_t count = reader.u32v();

		if (!reader.ok || count > payload.size())
			return false;

		events.reserve(count);

		for (uint32_t index = 0; index < count; index++)
		{
			const uint64_t epoch = oneEpoch ? sharedEpoch : reader.u64v();

			events.push_back(readEventV2(reader, epoch));

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
