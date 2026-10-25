#include "RtosGapExplainer.hpp"

#include <algorithm>

namespace
{
	RtosGapHint hint(RtosGapCause cause, std::string sentence, uint32_t count, uint32_t total = 0, double seconds = 0.0,
	                 double sharePercent = 0.0)
	{
		RtosGapHint result;

		result.cause = cause;
		result.sentence = std::move(sentence);
		result.count = count;
		result.total = total;
		result.seconds = seconds;
		result.sharePercent = sharePercent;

		return result;
	}

	/* What one missed record costs, in seconds, at the rate the session was
	   actually running at.
	   
	   Not the target's event rate: the records that were missed are exactly the
	   ones that would have raised it. The decoded ones are the sample, and the
	   window is the span they arrived over. Zero when there is no sample,
	   because a rate made up from nothing gives a duration made up from
	   nothing. */
	double secondsPerMissedRecord(const RtosGapEvidence& evidence)
	{
		if (evidence.recordsDecoded == 0 || evidence.windowSeconds <= 0.0)
			return 0.0;

		return evidence.windowSeconds / static_cast<double>(evidence.recordsDecoded);
	}

	double shareOfGap(double seconds, const RtosGapEvidence& evidence)
	{
		if (seconds <= 0.0 || evidence.unaccountedSeconds <= 0.0)
			return 0.0;

		const double share = seconds / evidence.unaccountedSeconds * 100.0;

		return share > 100.0 ? 100.0 : share;
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
		const double spacing = secondsPerMissedRecord(evidence);
		const double seconds = spacing * static_cast<double>(evidence.eventsMissed);

		std::string sentence = "this many records were written and never read, so interrupt intervals are missing their other end: " +
		                       std::to_string(evidence.eventsMissed);

		if (seconds > 0.0)
			sentence += ", about " + std::to_string(seconds).substr(0, 4) + " s of the gap";

		hints.push_back(hint(RtosGapCause::RecordsMissed, sentence, evidence.eventsMissed, 0, seconds, shareOfGap(seconds, evidence)));
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

	/* The biggest account first, and the ones with no number after them. A
	   reader with an incomplete observation wants to know where to look, and
	   "1374 records" and "0.9 s" are answers of different quality. */
	std::stable_sort(hints.begin(), hints.end(),
	                 [](const RtosGapHint& left, const RtosGapHint& right)
	                 {
		                 if (left.seconds != right.seconds)
			                 return left.seconds > right.seconds;

		                 return left.count > right.count;
	                 });

	return hints;
}
