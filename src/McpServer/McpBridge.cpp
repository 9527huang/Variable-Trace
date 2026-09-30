#include "McpBridge.hpp"

#include "McpTypes.hpp"

namespace mcp
{
	bool Bridge::runOnGuiThread(const std::function<void()>& task, std::chrono::milliseconds timeout)
	{
		if (!task || !accepting.load())
			return false;

		auto cancelled = std::make_shared<std::atomic<bool>>(false);
		auto completion = std::make_shared<std::promise<void>>();
		std::future<void> future = completion->get_future();

		{
			std::lock_guard<std::mutex> lock(mtx);

			if (!accepting.load())
				return false;

			tasks.push_back(Task{task, cancelled, completion});
		}

		if (future.wait_for(timeout) != std::future_status::ready)
		{
			/* The GUI thread may still pick the task up, so it is marked as
			   cancelled and the queue drain skips it. */
			cancelled->store(true);
			cancelledCount++;
			return false;
		}

		try
		{
			future.get();
		}
		catch (const std::exception&)
		{
			return false;
		}

		return true;
	}

	void Bridge::processPendingTasks()
	{
		std::deque<Task> local;

		{
			std::lock_guard<std::mutex> lock(mtx);

			if (tasks.empty())
				return;

			local.swap(tasks);
		}

		for (Task& task : local)
		{
			if (task.cancelled->load())
				continue;

			/* A tool must never be able to take the interface down, so every
			   failure is turned into a failed future instead of propagating. */
			try
			{
				task.callback();
				task.completion->set_value();
			}
			catch (...)
			{
				try
				{
					task.completion->set_exception(std::current_exception());
				}
				catch (...)
				{
				}
			}
		}
	}

	void Bridge::cancelWaitingTasks()
	{
		std::deque<Task> local;

		{
			std::lock_guard<std::mutex> lock(mtx);
			local.swap(tasks);
		}

		releaseTasks(local);
	}

	void Bridge::shutdown()
	{
		std::deque<Task> local;

		{
			std::lock_guard<std::mutex> lock(mtx);
			accepting.store(false);
			local.swap(tasks);
		}

		releaseTasks(local);
	}

	void Bridge::releaseTasks(std::deque<Task>& tasks)
	{
		/* The promise is failed rather than fulfilled: the task did not run, and a
		   caller that reads a value would report a success that never happened.
		   Failing it also wakes the caller immediately instead of leaving it to
		   sit out its timeout. */
		for (Task& task : tasks)
		{
			task.cancelled->store(true);

			try
			{
				task.completion->set_exception(std::make_exception_ptr(ToolError("The request was dropped before the application processed it.")));
			}
			catch (const std::exception&)
			{
				/* Satisfied already, nothing left to release. */
			}

			cancelledCount++;
		}
	}

	size_t Bridge::getCancelledCount() const
	{
		return cancelledCount.load();
	}
}  // namespace mcp
