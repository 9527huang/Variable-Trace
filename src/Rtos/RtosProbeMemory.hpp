#ifndef _RTOSPROBEMEMORY_HPP
#define _RTOSPROBEMEMORY_HPP

#include <cstdint>

#include "IDebugProbe.hpp"
#include "RtosMemory.hpp"

/*
 * Target memory as the probe sees it.
 *
 * The probe is handed over as a reference rather than looked up here, because
 * whoever builds this already holds the probe lock. Taking the lock in this
 * class would mean either taking it twice or taking it in a place the
 * acquisition loop did not mean to.
 *
 * The read count is kept because the snapshot budget is counted in round trips
 * and the round has to be able to say how many it spent.
 */
class RtosProbeMemory : public IRtosMemory
{
   public:
	explicit RtosProbeMemory(IDebugProbe& probe) : probe(probe)
	{
	}

	bool read(uint32_t address, uint8_t* buf, uint32_t size) override
	{
		readCount++;

		return probe.readBlock(address, buf, size);
	}

	uint32_t reads() const
	{
		return readCount;
	}

	void resetReads()
	{
		readCount = 0;
	}

   private:
	IDebugProbe& probe;
	uint32_t readCount = 0;
};

#endif
