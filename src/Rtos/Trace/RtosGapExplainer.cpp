#include "RtosGapExplainer.hpp"

#include <algorithm>

namespace
{
	RtosGapHint hint(RtosGapCause cause, std::string sentence, uint32_t count, uint32_t total = 0)
	{
		RtosGapHint result;

		result.cause = cause;
		result.sentence = std::move(sentence);
		result.count = count;
		result.total = total;

		return result;
	}
}  // namespace

std::vector<RtosGapHint> explainGap(const RtosGapEvidence& evidence)
{
	std::vector<RtosGapHint> hints;

	if (evidence.unaccountedPercent < gapExplainThresholdPercent)
		return hints;

	/* Ordered by how completely each cause explains a gap.
	   
	   Overwritten records come first because an interrupt interval needs both of
	   its events: losing one leaves a pair that never closes, and the time
	   between them is exactly what goes missing from the interrupt account.
	   Rejected rounds come next - a round that did not hold together leaves its
	   whole stretch without task state. Both are losses the session counted,
	   which is why they are named rather than guessed. */
	if (evidence.eventsMissed > 0)
	{
		hints.push_back(hint(RtosGapCause::RecordsMissed,
		                     "this many records were written and never read, so interrupt intervals are missing their other end: " +
		                         std::to_string(evidence.eventsMissed),
		                     evidence.eventsMissed));
	}

	if (evidence.roundsRejected > 0)
	{
		hints.push_back(hint(RtosGapCause::RoundsRejected,
		                     "this many rounds did not hold together, and their stretch has no task state: " +
		                         std::to_string(evidence.roundsRejected),
		                     evidence.roundsRejected));
	}

	if (evidence.tornBatches > 0)
	{
		hints.push_back(hint(RtosGapCause::TornBatches,
		                     "this many batches were abandoned mid-write and read again on the next round: " + std::to_string(evidence.tornBatches),
		                     evidence.tornBatches));
	}

	/* Estimated saves the shares rather than the gap: a task that was not read
	   this round is not a hole in time, it is a guess filling one. Worth saying
	   when it is a large share of what the window knows about. */
	if (evidence.tasksEstimated > 0 && evidence.tasksKnown > 0 && evidence.tasksEstimated * 3u >= evidence.tasksKnown)
	{
		hints.push_back(hint(RtosGapCause::TasksEstimated,
		                     "this many of the " + std::to_string(evidence.tasksKnown) + " known tasks were estimated rather than measured: " +
		                         std::to_string(evidence.tasksEstimated),
		                     evidence.tasksEstimated, evidence.tasksKnown));
	}

	/* Nothing in the evidence explains it. Saying that is more useful than
	   picking whichever cause happened to have a non-zero count, and it points
	   at the one explanation left: the rounds themselves are too far apart for
	   the states to be observed. */
	if (hints.empty())
	{
		hints.push_back(hint(RtosGapCause::Unexplained,
		                     "nothing in this session's counters explains it - the rounds are probably further apart than the states last", 0));
	}

	std::stable_sort(hints.begin(), hints.end(), [](const RtosGapHint& left, const RtosGapHint& right)
	                 {
		return left.count > right.count;
	});

	return hints;
}
