#ifndef _RTOSITMDECODER_HPP
#define _RTOSITMDECODER_HPP

#include <cstdint>
#include <vector>

#include "RtosEventRing.hpp"

/*
 * The records the target writes to a stimulus port, taken out of the trace
 * stream.
 *
 * SWO carries whatever the ITM puts on it: packets from every enabled port,
 * timestamps in between, and sync bytes where the stream was not running. A
 * record is eight bytes written one at a time, so each of them arrives as a
 * packet of its own and the record only exists once all eight are in. This
 * turns that stream back into records.
 *
 * The framing follows the decoder the trace view already uses on this target -
 * a software-source packet puts its port in bits 7:3 and its payload size in
 * bits 1:0, with three meaning four bytes; everything with a non-zero size code
 * that is not a source packet carries payload bytes of its own that have to be
 * stepped over, continuing while the continuation bit is set. Getting that
 * wrong does not fail loudly: it turns the following bytes into records.
 *
 * The port is one the trace view does not use, so that the two can share a
 * stream if they ever have to - the trace view drops a channel it does not know
 * rather than guessing at it.
 */
class RtosItmDecoder
{
   public:
	/* The stimulus port the target writes the records on. */
	static constexpr uint8_t recordPort = 1;

	/* Bytes in, in whatever sizes the probe hands them over. */
	void feed(const uint8_t* bytes, size_t size);

	/* Every whole record that has arrived and not been taken yet. A record the
	   stream was cut in the middle of stays here until its bytes come. */
	std::vector<RtosEventRing::Record> take();

	void clear();

   private:
	void consume(uint8_t byte);

	/* Bytes of records so far, in the order they arrived. */
	std::vector<uint8_t> recordBytes;

	/* Payload bytes still to come for the packet being read. */
	uint32_t remaining = 0;

	/* Whether those bytes belong to a record or to something to step over. */
	bool collecting = false;

	/* Inside a timestamp or another packet whose bytes are being stepped over. */
	bool skipping = false;
};

#endif
