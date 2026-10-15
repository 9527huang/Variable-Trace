#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
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

	/* The last argument names the logs this handler writes, so that the two
	   sides logging into one directory do not land on the same file. */
	DataHandlerBase(PlotGroupHandler* plotGroupHandler, VariableHandler* variableHandler, PlotHandler* plotHandler, PlotHandler* tracePlotHandler, std::atomic<bool>& done, std::mutex* mtx, spdlog::logger* logger, const std::string& logPrefix) : plotGroupHandler(plotGroupHandler), variableHandler(variableHandler), plotHandler(plotHandler), tracePlotHandler(tracePlotHandler), done(done), mtx(mtx), logger(logger)
	{
		csvStreamer = std::make_unique<CSVStreamer>(logger, logPrefix);
	}
	virtual ~DataHandlerBase() = default;

	virtual std::string getLastReaderError() const = 0;

	/* Whether the link to the target is open right now. A run that ends closes
	   it unless the reader asked for it to be kept, so the interface can say
	   which of the two it is looking at instead of guessing from the run. */
	virtual bool isProbeHeld() const { return false; }

	CSVStreamer::Metrics getCsvMetrics() const
	{
		return csvStreamer->getMetrics();
	}

	void setState(State state)
	{
		if (state == viewerState)
			return;

		viewerState = state;
		stateChangeOrdered = true;
		transitionStarted = std::chrono::steady_clock::now();
		stateChangeCondition.notify_all();
	}
	/* The state, as the interface asks for it: what was asked for, available at
	   once.

	   There is deliberately no accessor that waits for a pending transition to
	   be carried out. That transition contains the probe connect, so a caller
	   on the interface thread would be held for as long as the connect takes -
	   a fraction of a second on a healthy probe, tens of seconds on one that
	   was left in a bad state - and the window would stop drawing while it
	   waited. What the interface says about a transition is its own flag,
	   isTransitionPending(). */
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
	std::condition_variable stateChangeCondition;
	std::mutex stateChangeMutex;

	/* How long a change stays reported as in progress even after it has been
	   carried out, so that it is on screen long enough to be read. */
	static constexpr std::chrono::milliseconds minimumTransitionDisplay{400};

	/* Started well in the past, so that the first frame does not report a
	   transition that never happened. */
	std::chrono::steady_clock::time_point transitionStarted = std::chrono::steady_clock::now() - std::chrono::hours(1);

	spdlog::logger* logger;

	std::unique_ptr<CSVStreamer> csvStreamer;
};
