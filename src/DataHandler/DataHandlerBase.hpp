#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "CSVStreamer.hpp"
#include "PlotGroupHandler.hpp"
#include "PlotHandler.hpp"
#include "VariableHandler.hpp"
#include "spdlog/spdlog.h"

class DataHandlerBase
{
   public:
	enum class State
	{
		STOP = 0,
		RUN = 1,
	};

	DataHandlerBase(PlotGroupHandler* plotGroupHandler, VariableHandler* variableHandler, PlotHandler* plotHandler, PlotHandler* tracePlotHandler, std::atomic<bool>& done, std::mutex* mtx, spdlog::logger* logger) : plotGroupHandler(plotGroupHandler), variableHandler(variableHandler), plotHandler(plotHandler), tracePlotHandler(tracePlotHandler), done(done), mtx(mtx), logger(logger)
	{
		csvStreamer = std::make_unique<CSVStreamer>(logger);
	}
	virtual ~DataHandlerBase() = default;

	virtual std::string getLastReaderError() const = 0;

	void setState(State state)
	{
		if (state == viewerState)
			return;

		viewerState = state;
		stateChangeOrdered = true;
		transitionStarted = std::chrono::steady_clock::now();
	}
	State getState() const
	{
		/* TODO possible deadlock */
		while (stateChangeOrdered);
		return viewerState;
	}

	/* Reads the state without waiting for a pending transition to be processed.
	   getState() spins until the acquisition thread has finished the transition,
	   and that transition contains the probe connect, so a caller on the
	   interface thread would be held for as long as the connect takes. Callers
	   that only need to know whether acquisition runs use this one. */
	State getStateImmediate() const
	{
		return viewerState.load();
	}

	/* True between a state change being asked for and the acquisition thread
	   having carried it out. The requested state is stored straight away, so
	   without this an interface cannot tell a probe that is still being opened
	   from one that is already delivering data.
	 *
	 * A change to a probe that opens quickly is over in a few milliseconds, which
	 * is less than one frame, so a check that only looked at the flag would jump
	 * from one settled state to the other and the change would never be drawn at
	 * all. The answer therefore stays true for a short window measured from the
	 * request, long enough for a person to read the difference. */
	bool isTransitionPending() const
	{
		if (stateChangeOrdered.load())
			return true;

		return (std::chrono::steady_clock::now() - transitionStarted) < minimumTransitionDisplay;
	}

   protected:
	PlotGroupHandler* plotGroupHandler;
	VariableHandler* variableHandler;
	PlotHandler* plotHandler;
	PlotHandler* tracePlotHandler;
	std::atomic<bool>& done;
	std::atomic<State> viewerState = State::STOP;
	std::mutex* mtx;
	std::thread dataHandle;
	std::atomic<bool> stateChangeOrdered = false;

	/* How long a change stays reported as in progress even after it has been
	   carried out, so that it is on screen long enough to be read. */
	static constexpr std::chrono::milliseconds minimumTransitionDisplay{400};

	/* Started well in the past, so that the first frame does not report a
	   transition that never happened. */
	std::chrono::steady_clock::time_point transitionStarted = std::chrono::steady_clock::now() - std::chrono::hours(1);

	spdlog::logger* logger;

	std::unique_ptr<CSVStreamer> csvStreamer;
};