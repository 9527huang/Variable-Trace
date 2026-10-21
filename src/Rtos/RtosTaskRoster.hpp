#ifndef _RTOSTASKROSTER_HPP
#define _RTOSTASKROSTER_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "RtosSnapshot.hpp"

/*
 * Every task this session has seen, whether or not the newest round holds it.
 *
 * A round is a picture of an instant, and the kernel keeps changing while it is
 * being read: a task leaves a list between two reads, a walk of the delay list
 * comes back short, and a round of fourteen tasks is followed by one of eleven.
 * The table used to be built from whatever the round held, so rows appeared and
 * vanished under the reader - several times a second, in the middle of reading
 * them. Nothing about the target had changed enough to deserve it.
 *
 * What is kept here is the *identity* of a task - the things that do not change
 * while it lives: its name, the address of its control block, the size of its
 * stack, its base priority, its kernel number. The round supplies the readings;
 * this supplies the list of rows. A task a round did not read keeps its row and
 * every reading in it is shown as missing.
 *
 * The list only grows. A task that ends stays until the session does, which is
 * the point: "it was there a moment ago and this round cannot see it" is a fact
 * about the reading worth showing, and deleting the row states the opposite.
 * A new session starts a new roster - the addresses mean something else then.
 */
class RtosTaskRoster
{
   public:
	struct Entry
	{
		uint32_t tcbAddress = 0;

		std::string name;

		/* Which of the kernel's own lists it was last found on. Identity
		   enough for the detail block, which explains a state by it. */
		std::string sourceList;

		uint32_t stackBase = 0;
		uint32_t stackTop = 0;
		bool hasStack = false;

		/* Set when the task was created and only changed by someone who meant
		   to, which is what makes it usable as a stable order. */
		uint32_t basePriority = 0;
		bool hasBasePriority = false;

		uint32_t kernelTaskNumber = 0;
		bool hasKernelTaskNumber = false;

		/* The moment of the last round that held it, on the same clock the
		   view uses. "Not read this round" is worth a number as well as a
		   mark: a task seen two seconds ago and one seen two minutes ago are
		   not the same kind of quiet. */
		double lastSeenAt = 0.0;

		/* Rounds seen since the session began. A task that has appeared once
		   and never again is worth telling apart from one that has been in
		   every round until now. */
		uint32_t roundsSeen = 0;
	};

	/* Takes what the round says and adds what it holds. Never removes. */
	void observe(const RtosSnapshot& snapshot);

	void clear();

	const std::vector<Entry>& entries() const
	{
		return known;
	}

	size_t size() const
	{
		return known.size();
	}

	/* Null when the address is not in the roster. */
	const Entry* find(uint32_t tcbAddress) const;

   private:
	std::vector<Entry> known;
	std::unordered_map<uint32_t, size_t> index;
};

#endif
