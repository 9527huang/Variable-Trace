#ifndef _RTEC2FRAMES_HPP
#define _RTEC2FRAMES_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

/*
 * Frames of records, out of a byte stream (wire format §2.1).
 *
 * The ring gives the host slots it can address; a stream gives it bytes and no
 * boundaries. What restores the boundaries is this: 'R','T', a version, a
 * count, a length, a CRC over the payload, the payload, and a 0x0A that says
 * the frame really ended rather than the buffer running out.
 *
 * Everything here is about refusing: a stream that starts mid-frame, a frame
 * whose CRC does not match, a length that cannot be a frame, a payload that
 * does not hold exactly the records it claims. Each refusal costs the bytes up
 * to the next sync, and the counters say how many were spent - a stream that
 * resyncs twice a second is a broken link, not a quiet one.
 *
 * A frame is accepted only when all of it checks out. Half a frame of records
 * is not better than none: the sequence numbers would look continuous and the
 * history would join two stretches that never met.
 */
struct Rtec2Frame
{
	uint8_t version = 0;

	/* Records the writer said it put in. Checked against the payload before
	   the frame is handed over. */
	uint32_t count = 0;

	std::vector<uint8_t> payload;
};

class Rtec2FrameReader
{
   public:
	/* The frame header: 'R','T', version, count, length, crc. */
	static constexpr size_t headerBytes = 10;

	/* Wire format §2.1. A longer frame is not a frame whatever its CRC says. */
	static constexpr size_t maxPayloadBytes = 1024;

	/* How far the reader will look for a sync before it gives up on the bytes
	   it holds. A stream that has not produced a 'R','T' in this many bytes is
	   not carrying frames. */
	static constexpr size_t resyncWindow = 4096;

	void feed(const uint8_t* bytes, size_t size);
	void feed(const std::vector<uint8_t>& bytes)
	{
		feed(bytes.data(), bytes.size());
	}

	/* The frames that are whole and verified, oldest first. */
	std::vector<Rtec2Frame> take();

	struct Stats
	{
		uint32_t frames = 0;
		uint32_t crcFailures = 0;

		/* Bytes thrown away looking for the next sync. */
		uint64_t resyncBytes = 0;

		/* Frames whose declared length is impossible: zero, or over the cap. */
		uint32_t badLength = 0;

		/* Frames that ended in something other than 0x0A. */
		uint32_t badEnd = 0;

		/* Frames whose payload does not hold exactly `count` whole records. */
		uint32_t badPayload = 0;
	};

	const Stats& stats() const
	{
		return counters;
	}

	/* Bytes held that have not made a frame yet. */
	size_t pendingBytes() const
	{
		return buffer.size();
	}

	void clear();

	/* CRC-32/MPEG-2, the polynomial the wire format names. The check value for
	   "123456789" is 0x0376E6E7, and a test holds it to that: a CRC that only
	   agrees with itself proves nothing. */
	static uint32_t crc32(const uint8_t* bytes, size_t size);

   private:
	/* Parses whatever it can from the front of the buffer. */
	void parse();

	std::vector<uint8_t> buffer;
	std::vector<Rtec2Frame> ready;
	Stats counters;
};

#endif
