#ifndef _RTOSSAMPLER_HPP
#define _RTOSSAMPLER_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "IElfParser.hpp"
#include "RtosListWalker.hpp"
#include "RtosProfileBase.hpp"
#include "RtosSnapshot.hpp"

/*
 * One round of task state, read from a running target.
 *
 * The profile's table is turned into addresses once, and every round after
 * that reads those addresses. By the time a round runs, the kernel's names
 * have already become offsets and numbers, which is what keeps a second RTOS
 * from touching any of this.
 *
 * A round is either good enough to publish or it is not, and the sampler says
 * which. It does not publish: deciding what to do with a round that did not
 * hold together belongs to whoever owns the model, because only it knows what
 * the interface is currently showing.
 */
class RtosSampler
{
   public:
	struct Settings
	{
		/*
		 * How many ready lists the array holds.
		 *
		 * FreeRTOS takes this from `configMAX_PRIORITIES`, which is a compile
		 * time constant and never appears in a symbol file - so it cannot be
		 * read and has to be stated. The plan's boundary list calls this one
		 * out: reading past the end of the array would read whatever follows
		 * it in memory and report it as a priority with no tasks.
		 */
		uint32_t readyListCapacity = 5;

		/*
		 * How many slots the queue registry holds, and zero to leave
		 * synchronisation objects out of the round entirely.
		 *
		 * The same kind of number as the one above and for the same reason:
		 * FreeRTOS takes it from `configQUEUE_REGISTRY_SIZE`, a compile time
		 * constant that never appears in a symbol file. A kernel built with
		 * the registry switched off has no such array at all, and then there
		 * is nothing to enumerate.
		 */
		uint32_t queueRegistryCapacity = 8;

		/* Whether to read each task's control block. A round without it still
		   knows every task's address and state, which is the cheap scan; with
		   it, the round also has names, priorities and stack ranges. */
		bool readControlBlocks = true;

		RtosWalkLimits walkLimits;
	};

	/* Why a round could not be used. A round that did not hold together is not
	   published; it is recorded, so the interface can say a round went missing
	   instead of showing half of one. */
	struct Round
	{
		std::shared_ptr<RtosSnapshot> snapshot;

		/* Nothing could be read, or a field the walk needs has no offset.
		   Either way the round cannot stand, and `snapshot` is left null. */
		std::vector<std::string> problems;

		/* The target moved while the round was being taken. The round is
		   still published, and carries these so that whoever reads it knows
		   the parts are not all from one instant. */
		std::vector<std::string> inconsistencies;

		bool usable() const
		{
			return snapshot != nullptr;
		}
	};

	explicit RtosSampler(std::shared_ptr<RtosProfileBase> profile);

	/*
	 * Turns the profile's table into addresses using one symbol file.
	 *
	 * The layout is handed in rather than taken from the profile, because the
	 * offsets belong to the build and not to the kernel version: the caller
	 * reads them out of the same debug information the addresses came from,
	 * and the profile's own numbers only fill the gaps. False means a required
	 * symbol was missing, or a field the sampler needs has no offset at all;
	 * `lastError` says which.
	 */
	bool prepare(const IElfParser::SymbolMap& symbols, const RtosLayoutTable& layout, const Settings& settings);

	bool isPrepared() const
	{
		return prepared;
	}

	/* The sentence to show when the last `prepare` said no. */
	std::string lastError() const
	{
		return error;
	}

	/* The table of what the round resolved to, for the log and for a reader
	   that wants to see which symbols were found. */
	RtosProfileBase::Resolution resolution() const
	{
		return resolved;
	}

	/*
	 * Runs one round.
	 *
	 * The capture times are not set here. They are measured on the sampling
	 * loop's own clock, which this object has no access to, and the loop is
	 * the only place that knows when the round began and ended. It stamps them
	 * into the snapshot before publishing it.
	 */
	Round collect(IRtosMemory& memory, const std::string& sessionId, const Settings& settings) const;

	/*
	 * The lists whose addresses do not move, built during `prepare`: the ready
	 * array plus the suspended, pending and termination lists.
	 *
	 * The two delay lists are not here. They are reached through pointers the
	 * kernel swaps at a tick boundary, so they are read every round - and a
	 * round that read them once and reused the answer would follow a list the
	 * kernel had already left.
	 */
	const std::vector<RtosTaskEnumerator::ListSource>& listSources() const
	{
		return fixedListSources;
	}

   private:
	/* Reads several words, one address at a time, and stops at the first
	   failure. Used for the handful of globals a round needs before it can
	   walk anything. */
	bool readWords(IRtosMemory& memory, const std::vector<uint32_t>& addresses, std::vector<uint32_t>& values) const;

	/* Fills in what the control block holds and a list item does not. */
	bool readTaskDetail(IRtosMemory& memory, uint32_t tcbAddress, uint32_t span, RtosTask& task) const;

	std::shared_ptr<RtosProfileBase> profile;
	RtosProfileBase::Resolution resolved;

	/* The lists whose addresses do not move. The delay lists are not among
	   them: those are reached through pointers the kernel swaps, so they are
	   built per round instead. */
	std::vector<RtosTaskEnumerator::ListSource> fixedListSources;

	/* Addresses of the globals read every round. */
	uint32_t currentTaskSymbol = 0;
	uint32_t delayedListPointerSymbol = 0;
	uint32_t overflowDelayedListPointerSymbol = 0;

	/* The ready list array: the base address and how far it runs. */
	uint32_t readyListsAddress = 0;
	uint32_t readyListsStride = 0;

	/* The queue registry, or zero when the kernel was built without one. */
	uint32_t queueRegistryAddress = 0;

	/* Offsets and sizes taken from the layout during `prepare`. */
	uint32_t controlBlockSize = 0;
	uint32_t nameOffset = 0;
	uint32_t stackOffset = 0;
	uint32_t stackTopOffset = 0;
	uint32_t priorityOffset = 0;

	/* Where the event item's container pointer sits inside a control block: the
	   offset of the event item plus the offset of its container field. Absent
	   means the layout described neither, which costs a round the waiting column
	   and nothing else. */
	uint32_t eventListContainerOffset = 0xFFFFFFFFu;

	/* Where the run time counter sits, when the build has one. Absent on a
	   target built without run time statistics, which is a build option rather
	   than a failure: the column is simply not there. */
	uint32_t runTimeCounterOffset = 0xFFFFFFFFu;
	uint32_t basePriorityOffset = 0;
	uint32_t kernelNumberOffset = 0;
	uint32_t nameCapacity = 0;

	bool prepared = false;
	std::string error;
};

#endif
