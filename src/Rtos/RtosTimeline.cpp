#include "RtosTimeline.hpp"

#include <algorithm>

RtosTimeline::RtosTimeline(size_t capacity) : capacity(capacity < 1 ? 1 : capacity)
{
	samples.resize(this->capacity);
}

void RtosTimeline::add(const RtosSnapshot& snapshot)
{
	Sample sample;
	sample.atSeconds = snapshot.captureStartedAt;
	sample.entries.reserve(snapshot.tasks.size());

	for (const RtosTask& task : snapshot.tasks)
	{
		Entry entry;
		entry.tcbAddress = task.tcbAddress;
		entry.state = task.state;
		entry.hasCpuShare = task.hasCpuShare;
		entry.cpuSharePercent = task.cpuSharePercent;

		sample.entries.push_back(entry);
	}

	std::lock_guard<std::mutex> guard(mtx);

	samples[next] = std::move(sample);
	next = (next + 1) % capacity;
	filled = std::min(filled + 1, capacity);
}

void RtosTimeline::clear()
{
	std::lock_guard<std::mutex> guard(mtx);

	for (Sample& sample : samples)
		sample.entries.clear();

	next = 0;
	filled = 0;
}

void RtosTimeline::copyRange(double fromSeconds, double toSeconds, std::vector<Sample>& into) const
{
	into.clear();

	std::lock_guard<std::mutex> guard(mtx);

	if (filled == 0)
		return;

	/* The ring is in time order, oldest at `next` once it has wrapped. Walking
	   it costs one comparison per sample held, and the copy is only of what
	   falls inside the range. */
	const size_t oldest = (next + capacity - filled) % capacity;

	for (size_t index = 0; index < filled; index++)
	{
		const Sample& sample = samples[(oldest + index) % capacity];

		if (sample.atSeconds < fromSeconds)
			continue;

		if (sample.atSeconds > toSeconds)
			break;

		into.push_back(sample);
	}
}

size_t RtosTimeline::size() const
{
	std::lock_guard<std::mutex> guard(mtx);

	return filled;
}

double RtosTimeline::oldestSeconds() const
{
	std::lock_guard<std::mutex> guard(mtx);

	if (filled == 0)
		return 0.0;

	return samples[(next + capacity - filled) % capacity].atSeconds;
}

double RtosTimeline::newestSeconds() const
{
	std::lock_guard<std::mutex> guard(mtx);

	if (filled == 0)
		return 0.0;

	return samples[(next + capacity - 1) % capacity].atSeconds;
}
