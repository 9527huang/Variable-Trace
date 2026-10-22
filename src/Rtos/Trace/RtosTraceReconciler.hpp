#ifndef _RTOSTRACERECONCILER_HPP
#define _RTOSTRACERECONCILER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "RtosIdentityStore.hpp"
#include "RtosSnapshot.hpp"

/*
 * Where the two histories disagree.
 *
 * A round of the table is one reading of the target's lists; the event stream
 * is what the target said as it went. They see the same tasks from two
 * directions, and when they disagree the disagreement is the finding - not
 * something to resolve by picking the more convenient one.
 *
 * Three disagreements are worth reporting, and only three:
 *
 * - The snapshot shows a task that no event ever mentioned. Normal right after
 *   attaching (the creation happened before we started), and a problem if it
 *   persists: it means the events are not covering the target.
 * - An event entity is open and the snapshot does not show it. Either it was
 *   just created, or the round could not read it - `not-observed-in-snapshot`,
 *   which is not the same as deleted.
 * - The two have different names for one task. The snapshot's name wins for
 *   display and nothing else; history keeps what the target said at the time.
 *
 * Nothing here writes to either history: reconciling by editing history is how
 * a debugger ends up showing a target that never existed.
 */
struct RtosReconcileFinding
{
	enum class Kind
	{
		/* In the snapshot, never mentioned by an event. */
		SnapshotTaskNotInEvents,

		/* Open in the event history, absent from this round. */
		EventEntityNotInSnapshot,

		/* Both know it, under different names. */
		NameDiffers,
	};

	Kind kind = Kind::SnapshotTaskNotInEvents;
	uint32_t tcbAddress = 0;
	RtosEntityKey key;

	std::string snapshotName;
	std::string eventName;
};

class RtosTraceReconciler
{
   public:
	/* `epoch` is the current event epoch, used to build the key of a task the
	   events have never seen - which is the point: it has none. */
	static std::vector<RtosReconcileFinding> compare(const RtosSnapshot& snapshot, const RtosIdentityStore& identity, uint64_t epoch);
};

const char* toString(RtosReconcileFinding::Kind kind);

#endif
