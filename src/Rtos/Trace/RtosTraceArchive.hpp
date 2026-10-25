#ifndef _RTOSTRACEARCHIVE_HPP
#define _RTOSTRACEARCHIVE_HPP

#include <cstdint>
#include <vector>

#include "RtosIdentityStore.hpp"
#include "RtosTracePipeline.hpp"
#include "RtosTraceTypes.hpp"

/*
 * The events of a session, written down beside the rounds.
 *
 * A round is what the table shows; the events are what the lanes, the tracks,
 * the time budget and the fault list are made from. A file that held only
 * rounds reopens as a table with empty bands - the reader is left with the
 * numbers and none of the reasons.
 *
 * Three encodings, one block each:
 *
 * - **events** in the order they were applied, with the identity keys the
 *   pipeline had already decided. The keys are written rather than worked out
 *   again on the way in: which lifetime a handle belonged to is a decision made
 *   from a stream that is not in the file, and re-deciding it from a slice of
 *   that stream would put two different answers in one history.
 * - **identities**, so that a key can be turned back into a name.
 * - **the source**, so that the reader knows what the events came from and
 *   which tracks are even applicable.
 *
 * Every field is written one at a time, with the widths spelled out, for the
 * reason the round encoding spells them out: the format belongs to this pair of
 * files and not to the layout a struct happens to have this month. Sizes are
 * checked on the way in, and a payload that runs short is refused whole.
 */
namespace RtosTraceArchive
{
/* Bumped when any of the three layouts changes. A block from another version is
   skipped by its reader rather than decoded as far as it happens to make
   sense. */
constexpr uint16_t formatVersion = 3;

/*
 * Events are the bulk of a recording - 97.5% of the file on the wire - so the
 * layout is written for size before anything else.
 *
 * Version 2 stops writing what repeats: the epoch goes once per block instead
 * of once per event, a handle's kind and value are written once instead of
 * twice (the identity key and the wire reference agree about them; only the
 * generation can differ, and that case is flagged), the payload is written as
 * long as it is, and the display second is a float. Measured on a real session:
 * 114 bytes per event became 38.
 *
 * Version 3 compresses what version 2 produces. Records repeat themselves -
 * the same handle, the same quality, the same frequency - and a session of
 * forty thousand switches is mostly four handles taking turns. Measured on the
 * wire: 57 bytes an event became 21.
 *
 * Version 1 and 2 blocks are still read: a recording written before a change is
 * somebody's evidence, and a reader that refused it would be throwing away the
 * only copy.
 */
std::vector<uint8_t> encodeEvents(const std::vector<RtosNormalizedEvent>& events);

/* `version` is the block's, so that both layouts can be read. */
bool decodeEvents(const std::vector<uint8_t>& payload, std::vector<RtosNormalizedEvent>& events, uint16_t version = formatVersion);

std::vector<uint8_t> encodeIdentities(const RtosIdentityStore& identities);
bool decodeIdentities(const std::vector<uint8_t>& payload, RtosIdentityStore& identities);

std::vector<uint8_t> encodeSource(const RtosTraceSourceRecord& source);
bool decodeSource(const std::vector<uint8_t>& payload, RtosTraceSourceRecord& source);
}  // namespace RtosTraceArchive

#endif
