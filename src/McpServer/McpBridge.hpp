#ifndef _MCPBRIDGE_HPP
#define _MCPBRIDGE_HPP

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>

namespace mcp
{
	/*
	 * Carries a request from a connection thread over to the GUI thread.
	 *
	 * A tool call arrives on a connection thread, but the variable and plot model
	 * is owned by the GUI thread, so the call is queued and the connection thread
	 * waits for the application to run it. The wait is bounded on purpose: an
	 * open modal dialog or a stalled window must not pin a connection thread
	 * forever, and a request whose caller has already given up must not be
	 * applied minutes later.
	 */
	class Bridge
	{
	   public:
		static constexpr std::chrono::milliseconds defaultTimeout{15000};

		/*
		 * Runs the task on the GUI thread and returns once it finished. False
		 * means the task was cancelled: either it was never taken from the queue,
		 * or it is still queued and will be skipped.
		 */
		bool runOnGuiThread(const std::function<void()>& task, std::chrono::milliseconds timeout = defaultTimeout);

		/* Called by the application once per frame, on the GUI thread. */
		void processPendingTasks();

		/*
		 * Releases every caller that is still waiting for the GUI thread, without
		 * running its task. Used before the server is stopped or restarted: the
		 * stop joins the connection threads, and a thread parked on this queue
		 * would keep the join waiting for the whole request timeout.
		 */
		void cancelWaitingTasks();

		/* Called during shutdown so that waiting request threads are released. */
		void shutdown();

		/* Number of calls that timed out, for the status line. */
		size_t getCancelledCount() const;

	   private:
		struct Task
		{
			std::function<void()> callback;
			std::shared_ptr<std::atomic<bool>> cancelled;
			std::shared_ptr<std::promise<void>> completion;
		};

		/* Fails the promise of every task left in the deque and counts it as
		   cancelled. Shared by the two release paths. */
		void releaseTasks(std::deque<Task>& tasks);

		std::mutex mtx;
		std::deque<Task> tasks;
		std::atomic<bool> accepting{true};
		std::atomic<size_t> cancelledCount{0};
	};
}  // namespace mcp

#endif
