#ifndef _RTOSGAPEXPLAINER_HPP
#define _RTOSGAPEXPLAINER_HPP

#include <cstdint>
#include <string>
#include <vector>

/*
 * Why part of the window has no account.
 *
 * The time budget says how much of the window neither the tasks nor the
 * interrupts covered. That number on its own is a dead end: it says the
 * observation is incomplete without saying which part of the observation let it
 * down, and the four causes behind it are fixed in four different places.
 *
 * This turns the evidence the session already has into a short list of
 * sentences - ordered by how much of the gap each can account for - so that a
 * reader who sees "13.6% unaccounted" has somewhere to go next.
 *
 * It never guesses beyond the evidence: a cause with a zero count is not
 * mentioned, and when nothing in the evidence explains the gap the list says so
 * rather than picking the least unlikely one.
 */

struct RtosGapEvidence
{
	/* The gap itself, in percent of the window. Below the threshold the list is
	   empty: a small remainder is the sampling resolution, not a fault. */
	double unaccountedPercent = 0.0;

	/* Records written since the session began that the reader never decoded.
	   Each one is an event nobody saw, and an interrupt interval is two. */
	uint32_t eventsMissed = 0;

	/* Batches abandoned because a slot was caught mid-write. */
	uint32_t tornBatches = 0;

	/* Rounds the sampling loop refused to publish - the round did not hold
	   together, so that stretch has no task state at all. */
	uint32_t roundsRejected = 0;

	/* Tasks that had to be estimated rather than measured in the last round. */
	uint32_t tasksEstimated = 0;
	uint32_t tasksKnown = 0;
};

/*
 * Which counter a hint is about.
 *
 * The panel has to say these sentences in the reader's language, and a finished
 * English sentence cannot be translated by looking it up - each one carries a
 * number in the middle. So the cause travels as a value and the panel owns the
 * words: one switch there, one entry per cause in the translation table, and a
 * language the table does not cover still falls back to the English below.
 */
enum class RtosGapCause
{
	RecordsMissed,
	RoundsRejected,
	TornBatches,
	TasksEstimated,
	Unexplained,
};

struct RtosGapHint
{
	/* What the hint is about. Decides which sentence the panel prints. */
	RtosGapCause cause = RtosGapCause::Unexplained;

	/* The sentence in English, finished. The panel uses it when the reader has
	   not asked for another language, and the log uses it always. */
	std::string sentence;

	/* The number the sentence is about, so a caller can sort or filter without
	   parsing the text. */
	uint32_t count = 0;

	/* The whole a count is part of. Only the estimation hint has one: "5 of 10
	   tasks were estimated" needs both numbers to mean anything. Zero
	   elsewhere. */
	uint32_t total = 0;
};

/* Threshold under which the gap is not worth explaining, in percent. */
constexpr double gapExplainThresholdPercent = 5.0;

std::vector<RtosGapHint> explainGap(const RtosGapEvidence& evidence);

#endif
