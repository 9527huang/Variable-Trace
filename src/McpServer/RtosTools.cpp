#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "McpTools.hpp"
#include "RtosDiagnostics.hpp"
#include "RtosModel.hpp"
#include "ViewerDataHandler.hpp"

namespace mcp
{
	/*
	 * The findings as the panel draws them: the chain in the order the waiting
	 * goes, with the same task addresses the task list carries so that a caller
	 * can join the two.
	 */
	const char* findingKindName(RtosDiagnostics::Kind kind)
	{
		switch (kind)
		{
			case RtosDiagnostics::Kind::SuspectedDeadlock:
				return "suspected deadlock";
			case RtosDiagnostics::Kind::ReadyStarvation:
				return "ready, never switched in";
			case RtosDiagnostics::Kind::UnboundedWait:
				return "waiting with no timeout";
			default:
				return "priority inheritance";
		}
	}

	/* What the event side holds, or that there is no source. `missing` names the
	   capabilities this build does not have from it - an empty ISR track means
	   two different things depending on whether the carrier sends interrupts. */
	Json traceJson(const std::shared_ptr<RtosModel>& model)
	{
		if (model->trace() == nullptr || !model->trace()->attached())
			return Json::object({{"attached", false}});

		Json missing = Json::array();

		for (const std::string& name : model->trace()->missingCapabilities())
			missing.push_back(name);

		/* Counted here rather than kept as a second counter in the pipeline: the
		   two would disagree the first time one of them was updated and the
		   other was not. */
		uint32_t untimed = 0;

		for (const RtosNormalizedEvent& event : model->trace()->events())
		{
			if (!event.hasDisplaySeconds)
				untimed++;
		}

		return Json::object({{"attached", true},
		                     {"source", model->trace()->sourceName()},
		                     {"events", static_cast<uint32_t>(model->trace()->eventCount())},
		                     {"untimed", untimed},
		                     {"epoch", static_cast<uint32_t>(model->trace()->currentEpoch())},
		                     {"missing", missing}});
	}

	Json findingsJson(const RtosSnapshot* snapshot, const RtosTimeline& history)
	{
		Json findings = Json::array();

		if (snapshot == nullptr)
			return findings;

		for (const RtosDiagnostics::Finding& finding : RtosDiagnostics::diagnose(*snapshot, history))
		{
			findings.push_back(Json::object({{"kind", findingKindName(finding.kind)},
			                                 {"chain", finding.chain},
			                                 {"tasks", finding.tasks},
			                                 {"held_seconds", finding.heldSeconds},
			                                 {"held_rounds", finding.heldRounds}}));
		}

		return findings;
	}

	/*
	 * The recording and the file being played back.
	 *
	 * One object with both, because the caller's question is "what is the task
	 * view showing" and the answer is one or the other - never both.
	 */
	Json recordingJson(const RtosModel* model)
	{
		Json recording = Json::object({{"active", false}});
		Json playback = Json::object({{"active", false}});

		if (model == nullptr)
			return Json::object({{"recording", recording}, {"playback", playback}});

		if (model->isRecording())
		{
			const RtosRecording::Info info = model->recordingStats();

			recording["active"] = true;
			recording["path"] = model->recordingPath();
			recording["rounds"] = static_cast<uint64_t>(info.rounds);
			recording["bytes"] = info.bytes;
			recording["duration_s"] = info.durationSeconds();
		}

		if (model->isPlayback())
		{
			const RtosModel::PlaybackInfo info = model->playbackInfo();

			playback["active"] = true;
			playback["path"] = info.path;
			playback["rounds"] = static_cast<uint64_t>(info.rounds);
			playback["current"] = static_cast<uint64_t>(info.current);
			playback["first_s"] = info.firstSecond;
			playback["last_s"] = info.lastSecond;
		}

		return Json::object({{"recording", recording}, {"playback", playback}});
	}

	/* The model, or a sentence about why there is none. The task view exists
	   for every project; it only has rounds once it has prepared against one
	   built with the kernel symbols, which is the one thing worth saying. */
	std::shared_ptr<RtosModel> requireRtosModel(McpContext& context, const char* tool)
	{
		std::shared_ptr<RtosModel> model = context.viewerDataHandler != nullptr ? context.viewerDataHandler->getRtosModel() : nullptr;

		if (model == nullptr)
		{
			const std::string reason = context.viewerDataHandler != nullptr ? context.viewerDataHandler->getRtosPrepareError() : "";

			throw ToolError(std::string(tool) + " needs the task view, and it is not prepared: " + (reason.empty() ? "no reason was given" : reason));
		}

		return model;
	}

	Json startRtosRecording(McpContext& context, const Json& arguments)
	{
		requireApiWritesEnabled(context);

		const std::string path = requireString(arguments, "path");
		std::shared_ptr<RtosModel> model = requireRtosModel(context, "start_rtos_recording");

		RtosRecording::Problem problem = RtosRecording::Problem::None;

		if (!model->startRecording(path, problem))
			throw ToolError(std::string("The recording could not be started: ") + RtosRecording::describe(problem) + ".");

		return recordingJson(model.get());
	}

	Json stopRtosRecording(McpContext& context, const Json&)
	{
		requireApiWritesEnabled(context);

		std::shared_ptr<RtosModel> model = requireRtosModel(context, "stop_rtos_recording");

		/* The numbers before the file is closed: the counters live with the
		   handle, and closing it would leave the caller with nothing to report. */
		const RtosRecording::Info info = model->recordingStats();

		model->stopRecording();

		return Json::object({{"recording", Json::object({{"active", false},
		                                                 {"rounds", static_cast<uint64_t>(info.rounds)},
		                                                 {"bytes", info.bytes},
		                                                 {"duration_s", info.durationSeconds()}})}});
	}

	Json openRtosRecording(McpContext& context, const Json& arguments)
	{
		requireApiWritesEnabled(context);

		const std::string path = requireString(arguments, "path");
		std::shared_ptr<RtosModel> model = requireRtosModel(context, "open_rtos_recording");

		RtosRecording::Problem problem = RtosRecording::Problem::None;

		if (!model->openRecording(path, problem))
			throw ToolError(std::string("The recording could not be opened: ") + RtosRecording::describe(problem) + ".");

		return recordingJson(model.get());
	}

	Json closeRtosRecording(McpContext& context, const Json&)
	{
		requireApiWritesEnabled(context);

		std::shared_ptr<RtosModel> model = requireRtosModel(context, "close_rtos_recording");

		model->closePlayback();

		return recordingJson(model.get());
	}

	Json showRtosRound(McpContext& context, const Json& arguments)
	{
		requireApiWritesEnabled(context);

		std::shared_ptr<RtosModel> model = requireRtosModel(context, "show_rtos_round");

		if (!model->isPlayback())
			throw ToolError("show_rtos_round needs a recording to be open; open one with open_rtos_recording first.");

		/* A caller looking at a recording thinks in seconds, because that is what
		   the strip prints. The index is accepted too, for a caller that walked
		   the rounds itself. */
		const std::optional<double> atSeconds = optionalNumber(arguments, "at_s");
		const std::optional<int64_t> index = optionalInteger(arguments, "index");

		if (!atSeconds.has_value() && !index.has_value())
			throw ToolError("Field 'at_s' or 'index' is required.");

		const RtosModel::PlaybackInfo info = model->playbackInfo();

		size_t round = info.current;

		if (atSeconds.has_value())
			round = model->nearestPlaybackRound(*atSeconds);
		else if (index.has_value())
		{
			if (*index < 0 || static_cast<uint64_t>(*index) >= info.rounds)
				throw ToolError("Field 'index' is outside the recording, which holds " + std::to_string(info.rounds) + " round(s).");

			round = static_cast<size_t>(*index);
		}

		model->showPlaybackRound(round);

		return recordingJson(model.get());
	}

	/*
	 * The task view.
	 *
	 * Both tools are read only and both answer out of the snapshot the
	 * sampling loop last published. Neither reaches for the probe: a query
	 * that read the target would put a round trip in the middle of whatever
	 * the acquisition is doing, and the budget for those was spent when the
	 * snapshot was taken.
	 */
	namespace
	{
		constexpr uint32_t defaultTaskLimit = 100;
		constexpr uint32_t maxTaskLimit = 1000;

		std::string hexOf(uint32_t value)
		{
			char buffer[16] = {};

			std::snprintf(buffer, sizeof(buffer), "0x%08X", value);

			return buffer;
		}

		std::shared_ptr<RtosModel> requireModel(McpContext& context, const std::string& toolName)
		{
			if (context.viewerDataHandler == nullptr)
				reportUnavailable("the viewer data handler", toolName);

			/* Switched off is not the same as empty. Answering with no tasks
			   would read as a target that has none. */
			if (!context.viewerDataHandler->isRtosEnabled())
				throw ToolError("The task view is switched off. Set rtos.enabled in global.json and start a new acquisition.");

			const std::shared_ptr<RtosModel> model = context.viewerDataHandler->getRtosModel();

			if (model == nullptr)
				throw ToolError("The task view is switched on but was not prepared, so this run has no data. Start an acquisition with a project whose symbol file carries the kernel's symbols.");

			return model;
		}

		std::shared_ptr<const RtosSnapshot> requireSnapshot(McpContext& context, const std::string& toolName)
		{
			const std::shared_ptr<RtosModel> model = requireModel(context, toolName);
			const std::shared_ptr<const RtosSnapshot> snapshot = model->current();

			if (snapshot == nullptr)
				throw ToolError("The task view has not published a round yet. The first one runs shortly after the acquisition starts.");

			return snapshot;
		}

		/* A field the round did not read is reported as null rather than as
		   zero. Zero is a priority a task can have and an address a stack can
		   sit at, so it would be read as an answer. */
		Json optionalNumber(bool present, uint32_t value)
		{
			return present ? Json(value) : Json();
		}

		Json taskToJson(const RtosTask& task)
		{
			return Json::object({{"address", hexOf(task.tcbAddress)},
								 {"name", task.name},
								 {"state", toString(task.state)},
								 {"list", task.sourceList},
								 {"priority", optionalNumber(task.hasPriority, task.rawPriority)},
								 {"base_priority", optionalNumber(task.hasBasePriority, task.basePriority)},
								 {"kernel_task_number", optionalNumber(task.hasKernelTaskNumber, task.kernelTaskNumber)},
								 {"stack_base", task.hasStack ? Json(hexOf(task.stackBase)) : Json()},
								 {"stack_top", task.hasStack ? Json(hexOf(task.stackTop)) : Json()},
								 /* The distance between the two, which is the size the
								    task was created with. How much of it is in use
								    is a high water mark the kernel computes on
								    request, not a field in the block. */
								 {"stack_bytes", task.hasStack ? Json(task.stackTop - task.stackBase) : Json()},
								 /* The synchronisation object this task is blocked
								    on, or null when it is sitting on one of the
								    kernel's own lists. Null here means "not
								    waiting", which is an answer and not a gap. */
								 {"waiting_on", task.waitingOn.empty() ? Json() : Json(task.waitingOn)},
								 /* The list this task's event item is on: the object's own
								    wait list when it waits on one, and null when it is on no
								    object at all. Exposed so that a name can be checked
								    against the addresses list_sync_objects reports - the
								    comparison that tells a name that went missing from a
								    task that was never on the list. */
								 {"event_list", task.hasEventList ? Json(hexOf(task.eventList)) : Json()},
								 /* How much of the stack is free, read on this side from the
								    pattern a new stack is filled with. The age is beside it
								    because the sweep takes a few tasks a round, so the numbers
								    in one snapshot are not all from the same moment. A null free
								    count means there is no reading - either none was taken yet, or
								    the target does not fill its stacks, which `stack_scan` in
								    get_rtos_info tells apart. `reached_cap` true means the scan
								    stopped at its limit, so the used part is at most what it says. */
								 {"stack_free_bytes", task.hasStackWatermark ? Json(task.stackFreeBytes) : Json()},
								 {"stack_used_bytes",
								  task.hasStackWatermark ? Json(task.stackWatermarkStackBytes > task.stackFreeBytes ? task.stackWatermarkStackBytes - task.stackFreeBytes : 0u)
								                        : Json()},
								 {"stack_watermark_age_s", task.hasStackWatermark ? Json(task.stackWatermarkAgeSeconds) : Json()},
								 {"stack_watermark_reached_cap", task.hasStackWatermark ? Json(task.stackWatermarkReachedCap) : Json()},
								 /* The share still free, which is the number a stack alert is
								    about. The threshold is the caller's: the task view keeps
								    its own in the settings file, and an API client may want a
								    different one per target. */
								 {"stack_free_percent",
								  (task.hasStackWatermark && task.stackWatermarkStackBytes > 0)
								      ? Json(static_cast<uint32_t>((static_cast<uint64_t>(task.stackFreeBytes) * 100u) / task.stackWatermarkStackBytes))
								      : Json()},
								 /* This task's share of the time the kernel counted across all
								    tasks, and the window the snapshot reports. It is not a
								    share of the wall clock: the time the kernel attributes to
								    no task is not in the whole. Null means no share, which
								    includes the first round of a session. */
								 {"cpu_share_percent", task.hasCpuShare ? Json(task.cpuSharePercent) : Json()}});
		}

		Json rtosInfo(McpContext& context, const Json& args)
		{
			(void)args;

			/*
			 * The switch and the reason are read before the model is required.
			 * A task view that never prepared has no model, and why it did not
			 * is the one thing a caller asking about it needs - an exception
			 * saying "the task view is off" would be the least useful answer
			 * available.
			 */
			const bool enabled = context.viewerDataHandler->isRtosEnabled();
			const bool prepared = context.viewerDataHandler->isRtosPrepared();
			const std::string prepareError = context.viewerDataHandler->getRtosPrepareError();

			const std::shared_ptr<RtosModel> model = context.viewerDataHandler->getRtosModel();

			if (model == nullptr)
			{
				return Json::object({{"enabled", enabled},
									 {"prepared", prepared},
									 {"prepare_error", prepareError},
									 {"session_id", ""},
									 {"sequence", 0},
									 {"task_count", 0},
									 {"sync_object_count", 0},
									 {"rounds_published", 0},
									 {"rounds_rejected", 0},
									 {"last_rejection", ""},
									 {"problems", Json::array()},
								 {"findings", Json::array()},
								 {"events", Json::object({{"runs", 0}, {"lost", 0}})},
								 {"history_samples", 0},
								 {"history_oldest_s", 0.0},
								 {"stack_scan", Json::object({{"enabled", false}, {"fill_seen", false}, {"tasks_scanned", 0}, {"oldest_age_s", 0.0}})},
								 {"cpu_load", Json::object({{"supported", false}, {"counting", false}, {"tasks_measured", 0}, {"tasks_known", 0}, {"window_s", 0.0}})},
								 {"recording", recordingJson(nullptr)["recording"]},
								 {"playback", recordingJson(nullptr)["playback"]}});
			}

			const std::shared_ptr<const RtosSnapshot> snapshot = model->current();
			const RtosModel::Stats stats = model->stats();

			Json problems = Json::array();
			Json inconsistencies = Json::array();

			if (snapshot != nullptr)
			{
				for (const std::string& problem : snapshot->problems)
					problems.push_back(problem);

				/* Kept apart from the problems, and worth exposing for the
				   same reason the panel shows them: a round that caught the
				   target moving is a reading, and one that failed is not. A
				   caller that conflates the two throws away good rounds. */
				for (const std::string& note : snapshot->inconsistencies)
					inconsistencies.push_back(note);
			}

			return Json::object({{"enabled", enabled},
								 {"prepared", prepared},
								 {"prepare_error", prepareError},
								 {"session_id", model->sessionId()},
								 {"sequence", snapshot == nullptr ? 0 : snapshot->sequence},
								 {"consistency", snapshot == nullptr ? "no round yet" : toString(snapshot->consistency)},
								 {"capture_started_at_s", snapshot == nullptr ? 0.0 : snapshot->captureStartedAt},
								 {"capture_finished_at_s", snapshot == nullptr ? 0.0 : snapshot->captureFinishedAt},
								 {"capture_span_s", snapshot == nullptr ? 0.0 : snapshot->captureSpanSeconds},
								 {"task_count", snapshot == nullptr ? 0 : static_cast<uint32_t>(snapshot->taskCount())},
								 {"sync_object_count", snapshot == nullptr ? 0 : static_cast<uint32_t>(snapshot->syncObjectCount())},
								 {"rounds_published", stats.published},
								 {"rounds_rejected", stats.rejected},
								 {"last_rejection", stats.lastRejectionReason},
								 {"problems", problems},
								 {"inconsistencies", inconsistencies},
								 {"lists_walked", snapshot != nullptr ? Json(snapshot->listsWalked) : Json()},
								 {"lists_failed", snapshot != nullptr ? Json(snapshot->listsFailed) : Json()},
								 /* How far back the timeline reaches: how many rounds were kept
								    and the time of the oldest. A view can draw only what is
								    here, and a session that has run for hours has dropped the
								    beginning of itself. */
								 {"findings", findingsJson(snapshot.get(), model->history())},
								 /* Path B: how many runs the target's switch records have
								    given, and how many records never crossed. */
								 {"events",
								  Json::object({{"runs", static_cast<uint32_t>(model->events().size())},
								                {"lost", model->events().lostRecords()},
								                {"oldest_s", model->events().oldestSeconds()},
								                {"newest_s", model->events().newestSeconds()}})},
								 /* The event side, which the tracks page draws from.
								    Its own history rather than the runs above: that one is
								    what the lanes read, this one is what the tracks and the
								    findings will read, and they are fed by the same call
								    only so long as somebody remembers to. Reporting both
								    makes the difference visible. */
								 {"trace", traceJson(model)},
								 {"history_samples", static_cast<uint32_t>(model->history().size())},
								 {"history_oldest_s", model->history().oldestSeconds()},
								 /* The stack water marks are read a few tasks a round, so the
								    snapshot carries readings of several ages and covers part of
								    the set. `fill_seen` false while readings exist means the
								    target does not fill its stacks, which is a build option
								    rather than a fault: the column claims nothing then. */
								 {"stack_scan",
								  snapshot == nullptr ? Json() : Json(Json::object({{"enabled", snapshot->stackScan.enabled}, {"fill_seen", snapshot->stackScan.fillSeen}, {"tasks_scanned", static_cast<uint32_t>(snapshot->stackScan.tasksScanned)}, {"oldest_age_s", snapshot->stackScan.oldestReadingAgeSeconds}}))},
								 /* `supported` says the target keeps a run time counter at all,
								    which is a build option; `counting` says the counter moved
								    between the last two rounds. They are apart because the two
								    mean different things to whoever is reading: one is about
								    the firmware, the other about the target's timer. */
								 {"cpu_load",
								  snapshot == nullptr ? Json() : Json(Json::object({{"supported", snapshot->cpuLoad.supported}, {"counting", snapshot->cpuLoad.counting}, {"tasks_measured", static_cast<uint32_t>(snapshot->cpuLoad.tasksMeasured)}, {"tasks_known", static_cast<uint32_t>(snapshot->cpuLoad.tasksKnown)}, {"window_s", snapshot->cpuLoad.windowSeconds}}))},
								 {"recording", recordingJson(model.get())["recording"]},
								 {"playback", recordingJson(model.get())["playback"]}});
		}

		Json listTasks(McpContext& context, const Json& args)
		{
			const std::shared_ptr<const RtosSnapshot> snapshot = requireSnapshot(context, "list_tasks");

			uint32_t limit = defaultTaskLimit;

			if (args.contains("limit"))
			{
				if (!args["limit"].is_number_integer())
					throw ToolError("'limit' has to be a whole number.");

				const int64_t requested = args["limit"].get<int64_t>();

				if (requested <= 0)
					throw ToolError("'limit' has to be greater than zero.");

				limit = static_cast<uint32_t>(std::min<int64_t>(requested, maxTaskLimit));
			}

			Json tasks = Json::array();

			for (const RtosTask& task : snapshot->tasks)
			{
				if (tasks.size() >= limit)
					break;

				tasks.push_back(taskToJson(task));
			}

			Json problems = Json::array();

			for (const std::string& problem : snapshot->problems)
				problems.push_back(problem);

			/* The limits are reported beside the data rather than applied
			   silently, so a caller that asked for more than it got can tell
			   whether it got everything. */
			return Json::object({{"session_id", snapshot->sessionId},
								 {"sequence", snapshot->sequence},
								 {"consistency", toString(snapshot->consistency)},
								 {"capture_span_s", snapshot->captureSpanSeconds},
								 {"task_count", static_cast<uint32_t>(snapshot->taskCount())},
								 {"returned", static_cast<uint32_t>(tasks.size())},
								 {"limit", limit},
								 {"max_limit", maxTaskLimit},
								 {"problems", problems},
								 {"tasks", tasks}});
		}
		Json syncObjectToJson(const RtosSyncObject& object)
		{
			return Json::object({{"address", hexOf(object.address)},
								 {"name", object.name},
								 {"kind", toString(object.kind)},
								 {"capacity", optionalNumber(object.hasCounts, object.capacity)},
								 {"items", optionalNumber(object.hasCounts, object.items)},
								 {"waiting_to_send", optionalNumber(object.hasWaiters, object.waitingToSend)},
								 {"waiting_to_receive", optionalNumber(object.hasWaiters, object.waitingToReceive)},
								 {"holder", object.hasHolder ? Json(hexOf(object.holder)) : Json()},
								 {"recursive_count", optionalNumber(object.hasHolder, object.recursiveCount)},
								 /* The two wait lists, by address. Exposed so that
								    they can be compared against where the kernel
								    actually put them: a task's own list pointer is
								    matched against these, and an address that is
								    wrong but plausible reads as an empty list
								    rather than as an error. */
								 {"waiting_to_send_list", hexOf(object.waitingToSendList)},
								 {"waiting_to_receive_list", hexOf(object.waitingToReceiveList)}});
		}

		Json listSyncObjects(McpContext& context, const Json& args)
		{
			const std::shared_ptr<const RtosSnapshot> snapshot = requireSnapshot(context, "list_sync_objects");

			uint32_t limit = defaultTaskLimit;

			if (args.contains("limit"))
			{
				if (!args["limit"].is_number_integer())
					throw ToolError("'limit' has to be a whole number.");

				const int64_t requested = args["limit"].get<int64_t>();

				if (requested <= 0)
					throw ToolError("'limit' has to be greater than zero.");

				limit = static_cast<uint32_t>(std::min<int64_t>(requested, maxTaskLimit));
			}

			Json objects = Json::array();

			for (const RtosSyncObject& object : snapshot->syncObjects)
			{
				if (objects.size() >= limit)
					break;

				objects.push_back(syncObjectToJson(object));
			}

			Json problems = Json::array();

			for (const std::string& problem : snapshot->problems)
				problems.push_back(problem);

			/* An empty list here has two very different causes and the caller
			   cannot tell them apart from the list itself: a kernel with no
			   registry keeps no record of its queues at all, and a kernel with
			   one that nobody registered anything with has a record that is
			   empty. The snapshot carries which it was. */
			return Json::object({{"session_id", snapshot->sessionId},
								 {"sequence", snapshot->sequence},
								 {"registry_available", snapshot->queueRegistryAvailable},
								 {"object_count", static_cast<uint32_t>(snapshot->syncObjects.size())},
								 {"returned", static_cast<uint32_t>(objects.size())},
								 {"limit", limit},
								 {"max_limit", maxTaskLimit},
								 {"problems", problems},
								 {"objects", objects}});
		}
	}  // namespace

	void RtosTools::registerAll(ToolRegistry& registry)
	{
		registry.add({.name = "list_sync_objects",
					  .description = "Return the queues, semaphores and mutexes the kernel registered, as JSON: {session_id, sequence, "
									 "registry_available, object_count, returned, limit, max_limit, problems, objects}. Each object carries "
									 "{address, name, kind, capacity, items, waiting_to_send, waiting_to_receive, holder, recursive_count}. "
									 "A kernel built with its registry switched off keeps no record of its queues, so an empty list with "
									 "registry_available false is that and not a quiet target.",
					  .inputSchema = objectSchema(Json::object({{"limit", integerProperty("Most objects to return (default 100, at most 1000)")}})),
					  .handler = listSyncObjects});

		registry.add({.name = "get_rtos_info",
					  .description = "Return the state of the task view as JSON: {enabled, session_id, sequence, consistency, capture_started_at_s, "
									 "capture_finished_at_s, capture_span_s, task_count, sync_object_count, lists_walked, lists_failed, rounds_published, "
									 "rounds_rejected, last_rejection, stack_scan, cpu_load, history_samples, history_oldest_s, problems, inconsistencies, recording, playback}. "
									 "cpu_load carries {supported, counting, tasks_measured, tasks_known, window_s}. "
									 "stack_scan carries {enabled, fill_seen, tasks_scanned, oldest_age_s}. "
					 "recording carries {active, path, rounds, bytes, duration_s} while a recording is being written; playback carries "
					 "{active, path, rounds, current, first_s, last_s} while a file is being shown instead of the target. "
									 "Reads the last published snapshot; it does not touch the target.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = rtosInfo});

		registry.add({.name = "start_rtos_recording",
					  .description = "Write every round the task view is shown to a file, so that the same table, findings and lanes can be read "
									 "later with no probe attached. Returns {recording, playback}. The file is a snapshot of the view, not a second "
									 "model of the target; nothing is read from the target because of it. Requires API writes.",
					  .inputSchema = objectSchema(Json::object({{"path", stringProperty("File to write, for example D:/captures/run.vtrec")}}), {"path"}),
					  .handler = startRtosRecording});

		registry.add({.name = "stop_rtos_recording",
					  .description = "Close the file being recorded and report what went into it: {recording: {active, rounds, bytes, duration_s}}. "
									 "Requires API writes.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = stopRtosRecording});

		registry.add({.name = "open_rtos_recording",
					  .description = "Show a recording instead of a live target: the rounds go into the timeline and the last one becomes the round "
									 "the task tools answer from. Starting an acquisition closes it. Returns {recording, playback}. Requires API writes.",
					  .inputSchema = objectSchema(Json::object({{"path", stringProperty("Recording written by start_rtos_recording")}}), {"path"}),
					  .handler = openRtosRecording});

		registry.add({.name = "close_rtos_recording",
					  .description = "Stop showing a recording and go back to the live target: the history is cleared and the task tools report no "
									 "snapshot until an acquisition publishes one. Requires API writes.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = closeRtosRecording});

		registry.add({.name = "show_rtos_round",
					  .description = "In a recording, make one round the round the task tools answer from. Give 'at_s' (the same seconds the strip "
									 "prints; the nearest round is used) or 'index' (0 based). Returns {recording, playback} with the round now current. "
									 "Requires API writes.",
					  .inputSchema = objectSchema(Json::object({{"at_s", numberProperty("Seconds into the recording")},
					                                            {"index", integerProperty("Round number, 0 based")}})),
					  .handler = showRtosRound});

		registry.add({.name = "list_tasks",
					  .description = "Return the tasks of the most recent snapshot as JSON: {session_id, sequence, consistency, capture_span_s, "
									 "task_count, returned, limit, max_limit, problems, tasks}. Each task carries {address, name, state, list, "
									 "priority, base_priority, kernel_task_number, stack_base, stack_top, stack_bytes, waiting_on, event_list, "
									 "stack_free_bytes, stack_used_bytes, stack_free_percent, stack_watermark_age_s, stack_watermark_reached_cap, cpu_share_percent}. A field the round did not "
									 "read is null rather than zero.",
					  .inputSchema = objectSchema(Json::object({{"limit", integerProperty("Most tasks to return (default 100, at most 1000)")}})),
					  .handler = listTasks});
	}
}  // namespace mcp
