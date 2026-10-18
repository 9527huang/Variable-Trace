#ifndef _GUIRTOSSELECTION_HPP
#define _GUIRTOSSELECTION_HPP

#include <cstdint>
#include <string>

/*
 * What the two halves of the task view are pointing at.
 *
 * The table that says what is true now and the timeline that says how it got
 * there sit in two windows, and the two have to agree on one thing: which task
 * the reader picked. So the addresses live here rather than inside either
 * panel - a field in one panel would be a field the other cannot see, and the
 * two windows are drawn by two different calls.
 *
 * It is a small piece of shared state rather than a global registry: nothing
 * else in the program has any use for it, and one object with four fields is
 * easier to follow than a singleton with an interface.
 *
 * Nothing here is written from anywhere but the GUI thread.
 */
struct GuiRtosSelection
{
	/* The row that was clicked, by control block address. Zero means nothing
	   is selected - no task has that address. */
	uint32_t task = 0;

	/* The row the mouse is on, for this frame only. The table sets it and the
	   timeline reads it; a frame in which the table was not drawn leaves it
	   at zero, so a lane is never left lit with nothing pointing at it. */
	uint32_t hoveredTask = 0;
	bool tableDrewThisFrame = false;

	/* The object the objects table points at, and the one it last scrolled
	   to - scrolling on every frame would fight the wheel. */
	std::string object;
	std::string scrolledObject;

	static GuiRtosSelection& shared()
	{
		/* One per process. The panels are static classes with no instance of
		   their own, and this is the piece of state they have in common. */
		static GuiRtosSelection selection;

		return selection;
	}
};

#endif
