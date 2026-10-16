#ifndef _RTOSMEMORY_HPP
#define _RTOSMEMORY_HPP

#include <cstdint>

/*
 * The whole of a probe that this layer needs.
 *
 * Everything below the sampler goes through one method, and that is what lets
 * a walk over a kernel list be tested against a memory image instead of a
 * board. A chain that turns into a loop, a node whose backward pointer lies
 * about where it came from, a list that disagrees with its own item count -
 * those are the inputs the walk has to survive, and a running target cannot be
 * asked to produce them on demand.
 */
class IRtosMemory
{
   public:
	virtual ~IRtosMemory() = default;

	/* Reads `size` bytes from `address`. False means the read failed, and the
	   caller has to abandon the round rather than keep what it has: half a
	   snapshot presented as a whole one is exactly the kind of quiet wrong
	   answer the consistency levels exist to prevent. */
	virtual bool read(uint32_t address, uint8_t* buf, uint32_t size) = 0;
};

#endif
