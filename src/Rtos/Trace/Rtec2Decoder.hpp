#ifndef _RTEC2DECODER_HPP
#define _RTEC2DECODER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "RtosTraceTypes.hpp"

/*
 * Bytes to events, following docs/RTEC2-wire-format.md §3.
 *
 * Deliberately dumb: it validates lengths, maps types, and hands back what the
 * bytes say. It does not decide identities, extend ticks or judge quality -
 * those belong to the pipeline, and a decoder that also interpreted would be a
 * second opinion about the target.
 *
 * Two things it must never do:
 *
 * - **Guess a type.** An unknown record is skipped and counted. A decoder that
 *   guessed would turn a newer target's field into a value of an older one.
 * - **Read past the buffer.** A record that runs off the end is reported as
 *   truncated; the caller keeps what came before it and knows the rest is not
 *   missing but absent.
 */
struct Rtec2DecodedRecord
{
	RtosTraceEvent event;

	uint8_t payloadBytes = 0;
	uint8_t payload[12] = {};
};

struct Rtec2DecodeResult
{
	std::vector<Rtec2DecodedRecord> records;

	/* Records whose type this build does not know. Skipped, not an error. */
	uint32_t unknownTypes = 0;

	/* Bytes at the end that did not make a whole record. Non zero means the
	   file or the read stopped in the middle of one. */
	size_t truncatedBytes = 0;
};

class Rtec2Decoder
{
   public:
	/* How many bytes one record occupies, or 0 when the bytes do not describe
	 *a record that fits. */
	static size_t recordBytes(const uint8_t* bytes, size_t size);

	static Rtec2DecodeResult decode(const uint8_t* bytes, size_t size);

	static Rtec2DecodeResult decode(const std::vector<uint8_t>& bytes)
	{
		return decode(bytes.data(), bytes.size());
	}

	/* The namespaces from §3: which entities a type's two handles refer to.
	 * Kept here so that the decoders in three languages do not each keep a
	 * table of their own. */
	static RtosEntityKind subjectKind(RtosTraceEventType type);
	static RtosEntityKind objectKind(RtosTraceEventType type);

	/* The type a byte means, or Unknown. */
	static RtosTraceEventType typeOf(uint8_t value);
};

#endif
