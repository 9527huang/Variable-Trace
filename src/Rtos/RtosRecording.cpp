#include "RtosRecording.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace
{
	/* The first eight bytes say which format this is, so that a project file or
	   a CSV dropped on the dialog is refused with a sentence rather than parsed
	   into numbers that happen to make sense. */
	const char fileMagic[8] = {'V', 'T', 'R', 'E', 'C', 'O', 'R', 'D'};

	/* One record begins with this and its length, which is what lets a reader
	   skip a record it cannot use instead of walking into the middle of one. */
	const uint32_t recordMagic = 0x52444E52;  /* 'RNDR' */

	/* header: magic (8) + version (4) + reserved (4) */
	constexpr size_t headerBytes = 16;

	/* record: magic (4) + payload bytes (4) */
	constexpr size_t recordHeaderBytes = 8;

	/* Appends the fields in the order the reader expects them. */
	struct Sink
	{
		std::vector<uint8_t>& bytes;

		void u8(uint8_t value)
		{
			bytes.push_back(value);
		}

		void u32(uint32_t value)
		{
			bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&value), reinterpret_cast<const uint8_t*>(&value) + sizeof(value));
		}

		void u64(uint64_t value)
		{
			bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&value), reinterpret_cast<const uint8_t*>(&value) + sizeof(value));
		}

		void d(double value)
		{
			bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&value), reinterpret_cast<const uint8_t*>(&value) + sizeof(value));
		}

		void text(const std::string& value)
		{
			u32(static_cast<uint32_t>(value.size()));
			bytes.insert(bytes.end(), value.begin(), value.end());
		}
	};

	/*
	 * Reads the same fields back.
	 *
	 * Every read checks that the bytes are there: a file that was cut short -
	 * the target lost power, the machine was switched off mid-write - must come
	 * back as "this round is not readable" and not as an out of bounds read.
	 * Once `ok` is false every further read is a no-op, so a caller can decode a
	 * whole round and ask once at the end.
	 */
	struct Source
	{
		const uint8_t* bytes = nullptr;
		size_t size = 0;
		size_t pos = 0;
		bool ok = true;

		bool has(size_t count)
		{
			if (!ok)
				return false;

			if (pos + count > size)
			{
				ok = false;

				return false;
			}

			return true;
		}

		uint8_t u8()
		{
			if (!has(1))
				return 0;

			return bytes[pos++];
		}

		uint32_t u32()
		{
			if (!has(sizeof(uint32_t)))
				return 0;

			uint32_t value = 0;
			std::memcpy(&value, bytes + pos, sizeof(value));
			pos += sizeof(value);

			return value;
		}

		uint64_t u64()
		{
			if (!has(sizeof(uint64_t)))
				return 0;

			uint64_t value = 0;
			std::memcpy(&value, bytes + pos, sizeof(value));
			pos += sizeof(value);

			return value;
		}

		double d()
		{
			if (!has(sizeof(double)))
				return 0.0;

			double value = 0.0;
			std::memcpy(&value, bytes + pos, sizeof(value));
			pos += sizeof(value);

			return value;
		}

		std::string text()
		{
			const uint32_t length = u32();

			/* A length that cannot be in this record means the record is not
			   what it claimed: refuse it rather than trying to allocate it. */
			if (!has(length))
			{
				ok = false;

				return {};
			}

			std::string value(reinterpret_cast<const char*>(bytes + pos), length);
			pos += length;

			return value;
		}
	};

	void writeTask(Sink& sink, const RtosTask& task)
	{
		sink.u32(task.tcbAddress);
		sink.u32(task.stateListItem);
		sink.u32(task.waitingList);
		sink.u8(static_cast<uint8_t>(task.state));

		sink.u32(task.rawPriority);
		sink.u8(task.hasPriority ? 1 : 0);
		sink.u64(static_cast<uint64_t>(task.priorityRank));

		sink.u32(task.waitValue);
		sink.u8(task.hasWaitTime ? 1 : 0);

		sink.u32(task.eventList);
		sink.u8(task.hasEventList ? 1 : 0);

		sink.u32(task.stackBase);
		sink.u32(task.stackTop);
		sink.u8(task.hasStack ? 1 : 0);

		sink.u8(task.hasStackWatermark ? 1 : 0);
		sink.u32(task.stackFreeBytes);
		sink.u32(task.stackWatermarkStackBytes);
		sink.d(task.stackWatermarkAgeSeconds);
		sink.u8(task.stackWatermarkReachedCap ? 1 : 0);

		sink.u32(task.runTimeCounter);
		sink.u8(task.hasRunTimeCounter ? 1 : 0);

		sink.u8(task.hasCpuShare ? 1 : 0);
		sink.d(task.cpuSharePercent);

		sink.u32(task.basePriority);
		sink.u8(task.hasBasePriority ? 1 : 0);

		sink.u32(task.kernelTaskNumber);
		sink.u8(task.hasKernelTaskNumber ? 1 : 0);

		sink.text(task.name);
		sink.text(task.waitingOn);
		sink.text(task.sourceList);
	}

	RtosTask readTask(Source& source)
	{
		RtosTask task;

		task.tcbAddress = source.u32();
		task.stateListItem = source.u32();
		task.waitingList = source.u32();
		task.state = static_cast<RtosTaskState>(source.u8());

		task.rawPriority = source.u32();
		task.hasPriority = source.u8() != 0;
		task.priorityRank = static_cast<int64_t>(source.u64());

		task.waitValue = source.u32();
		task.hasWaitTime = source.u8() != 0;

		task.eventList = source.u32();
		task.hasEventList = source.u8() != 0;

		task.stackBase = source.u32();
		task.stackTop = source.u32();
		task.hasStack = source.u8() != 0;

		task.hasStackWatermark = source.u8() != 0;
		task.stackFreeBytes = source.u32();
		task.stackWatermarkStackBytes = source.u32();
		task.stackWatermarkAgeSeconds = source.d();
		task.stackWatermarkReachedCap = source.u8() != 0;

		task.runTimeCounter = source.u32();
		task.hasRunTimeCounter = source.u8() != 0;

		task.hasCpuShare = source.u8() != 0;
		task.cpuSharePercent = source.d();

		task.basePriority = source.u32();
		task.hasBasePriority = source.u8() != 0;

		task.kernelTaskNumber = source.u32();
		task.hasKernelTaskNumber = source.u8() != 0;

		task.name = source.text();
		task.waitingOn = source.text();
		task.sourceList = source.text();

		return task;
	}

	void writeObject(Sink& sink, const RtosSyncObject& object)
	{
		sink.u32(object.address);
		sink.u8(static_cast<uint8_t>(object.kind));

		sink.u32(object.capacity);
		sink.u32(object.items);
		sink.u8(object.hasCounts ? 1 : 0);

		sink.u32(object.waitingToSend);
		sink.u32(object.waitingToReceive);
		sink.u8(object.hasWaiters ? 1 : 0);

		sink.u32(object.waitingToSendList);
		sink.u32(object.waitingToReceiveList);

		sink.u32(object.holder);
		sink.u32(object.recursiveCount);
		sink.u8(object.hasHolder ? 1 : 0);

		sink.text(object.name);
	}

	RtosSyncObject readObject(Source& source)
	{
		RtosSyncObject object;

		object.address = source.u32();
		object.kind = static_cast<RtosSyncKind>(source.u8());

		object.capacity = source.u32();
		object.items = source.u32();
		object.hasCounts = source.u8() != 0;

		object.waitingToSend = source.u32();
		object.waitingToReceive = source.u32();
		object.hasWaiters = source.u8() != 0;

		object.waitingToSendList = source.u32();
		object.waitingToReceiveList = source.u32();

		object.holder = source.u32();
		object.recursiveCount = source.u32();
		object.hasHolder = source.u8() != 0;

		object.name = source.text();

		return object;
	}
}  // namespace

RtosRecording::~RtosRecording()
{
	finish();
	close();
}

bool RtosRecording::begin(const std::string& path, Problem& problem)
{
	problem = Problem::None;

	if (reading)
	{
		problem = Problem::AnotherRecordingIsOpen;

		return false;
	}

	if (writing)
		finish();

	file = std::fopen(path.c_str(), "wb");

	if (file == nullptr)
	{
		problem = Problem::CannotCreate;

		return false;
	}

	/* The rounds arrive a hundred times a second in small blocks; without a
	   buffer that is a hundred small writes a second through the C library. */
	std::setvbuf(file, nullptr, _IOFBF, 64 * 1024);

	uint8_t header[headerBytes] = {};
	std::memcpy(header, fileMagic, sizeof(fileMagic));
	std::memcpy(header + 8, &formatVersion, sizeof(formatVersion));

	if (std::fwrite(header, 1, sizeof(header), file) != sizeof(header))
	{
		std::fclose(file);
		file = nullptr;
		problem = Problem::CannotCreate;

		return false;
	}

	{
		std::lock_guard<std::mutex> lock(mtx);

		filePath = path;
		roundsWritten = 0;
		firstWrittenSecond = 0.0;
		lastWrittenSecond = 0.0;
		haveFirstSecond = false;
		writing = true;
	}

	return true;
}

void RtosRecording::write(const RtosSnapshot& snapshot, const std::vector<RtosEventRing::Interval>& events)
{
	if (!writing)
		return;

	buffer.clear();
	buffer.reserve(1024 + snapshot.tasks.size() * 96 + snapshot.syncObjects.size() * 48);

	Sink sink{buffer};

	sink.d(snapshot.captureStartedAt);
	sink.d(snapshot.captureFinishedAt);
	sink.d(snapshot.captureSpanSeconds);
	sink.u8(static_cast<uint8_t>(snapshot.consistency));
	sink.u32(snapshot.listsWalked);
	sink.u32(snapshot.listsFailed);

	sink.u8(snapshot.queueRegistryAvailable ? 1 : 0);

	sink.u8(snapshot.stackScan.enabled ? 1 : 0);
	sink.u8(snapshot.stackScan.fillSeen ? 1 : 0);
	sink.u32(snapshot.stackScan.tasksScanned);
	sink.d(snapshot.stackScan.oldestReadingAgeSeconds);

	sink.u8(snapshot.cpuLoad.supported ? 1 : 0);
	sink.u8(snapshot.cpuLoad.counting ? 1 : 0);
	sink.u32(snapshot.cpuLoad.tasksEstimated);
	sink.u32(snapshot.cpuLoad.tasksMeasured);
	sink.u32(snapshot.cpuLoad.tasksKnown);
	sink.d(snapshot.cpuLoad.windowSeconds);

	sink.text(snapshot.sessionId);

	sink.u32(static_cast<uint32_t>(snapshot.problems.size()));

	for (const std::string& note : snapshot.problems)
		sink.text(note);

	sink.u32(static_cast<uint32_t>(snapshot.inconsistencies.size()));

	for (const std::string& note : snapshot.inconsistencies)
		sink.text(note);

	sink.u32(static_cast<uint32_t>(snapshot.layerAges.size()));

	for (const RtosLayerAge& age : snapshot.layerAges)
	{
		sink.text(age.layer);
		sink.d(age.ageSeconds);
	}

	sink.u32(static_cast<uint32_t>(snapshot.tasks.size()));

	for (const RtosTask& task : snapshot.tasks)
		writeTask(sink, task);

	sink.u32(static_cast<uint32_t>(snapshot.syncObjects.size()));

	for (const RtosSyncObject& object : snapshot.syncObjects)
		writeObject(sink, object);

	/* The switch records that arrived with this round. Written last so that a
	   file from before they existed is a record that ends here, which is
	   exactly what the reader of a version 1 file expects. */
	sink.u32(static_cast<uint32_t>(events.size()));

	for (const RtosEventRing::Interval& run : events)
	{
		sink.u32(run.tcbAddress);
		sink.d(run.fromSeconds);
		sink.d(run.toSeconds);
	}

	uint8_t recordHeader[recordHeaderBytes] = {};
	const uint32_t payloadBytes = static_cast<uint32_t>(buffer.size());
	std::memcpy(recordHeader, &recordMagic, sizeof(recordMagic));
	std::memcpy(recordHeader + 4, &payloadBytes, sizeof(payloadBytes));

	std::lock_guard<std::mutex> lock(mtx);

	if (file == nullptr)
		return;

	if (std::fwrite(recordHeader, 1, sizeof(recordHeader), file) != sizeof(recordHeader) ||
	    std::fwrite(buffer.data(), 1, buffer.size(), file) != buffer.size())
	{
		/* A file that stopped taking bytes is finished; the counters stay where
		   they were rather than claiming a round that is not in the file. */
		std::fclose(file);
		file = nullptr;
		writing = false;

		return;
	}

	roundsWritten++;

	if (!haveFirstSecond)
	{
		firstWrittenSecond = snapshot.captureStartedAt;
		haveFirstSecond = true;
	}

	lastWrittenSecond = snapshot.captureStartedAt;
}

void RtosRecording::finish()
{
	std::FILE* closing = nullptr;

	{
		std::lock_guard<std::mutex> lock(mtx);

		closing = file;
		file = nullptr;
		writing = false;
	}

	if (closing != nullptr)
	{
		std::fflush(closing);
		std::fclose(closing);
	}
}

RtosRecording::Info RtosRecording::stats() const
{
	std::lock_guard<std::mutex> lock(mtx);

	Info info;
	info.rounds = static_cast<size_t>(roundsWritten);
	info.firstSecond = firstWrittenSecond;
	info.lastSecond = lastWrittenSecond;

	if (file != nullptr)
		info.bytes = static_cast<uint64_t>(std::ftell(file));

	return info;
}

bool RtosRecording::open(const std::string& path, Problem& problem)
{
	problem = Problem::None;

	if (writing)
	{
		problem = Problem::ARecordingIsBeingWritten;

		return false;
	}

	if (reading)
		close();

	std::FILE* opened = std::fopen(path.c_str(), "rb");

	if (opened == nullptr)
	{
		problem = Problem::CannotOpen;

		return false;
	}

	uint8_t header[headerBytes] = {};

	if (std::fread(header, 1, sizeof(header), opened) != sizeof(header) || std::memcmp(header, fileMagic, sizeof(fileMagic)) != 0)
	{
		std::fclose(opened);
		problem = Problem::NotARecording;

		return false;
	}

	uint32_t version = 0;
	std::memcpy(&version, header + 8, sizeof(version));

	if (version < oldestReadableVersion || version > formatVersion)
	{
		std::fclose(opened);
		problem = Problem::WrittenByAnotherVersion;

		return false;
	}

	/* Where the ends are, for the index that follows. */
	std::fseek(opened, 0, SEEK_END);
	const long fileBytes = std::ftell(opened);
	std::fseek(opened, static_cast<long>(headerBytes), SEEK_SET);

	std::vector<IndexEntry> scanned;

	while (std::ftell(opened) + static_cast<long>(recordHeaderBytes) <= fileBytes)
	{
		uint8_t recordHeader[recordHeaderBytes] = {};

		if (std::fread(recordHeader, 1, sizeof(recordHeader), opened) != sizeof(recordHeader))
			break;

		uint32_t magic = 0;
		uint32_t payloadBytes = 0;
		std::memcpy(&magic, recordHeader, sizeof(magic));
		std::memcpy(&payloadBytes, recordHeader + 4, sizeof(payloadBytes));

		if (magic != recordMagic || payloadBytes == 0)
			break;

		const long payloadStart = std::ftell(opened);

		if (payloadStart + static_cast<long>(payloadBytes) > fileBytes)
			break;

		/* The first field of a record is the moment it was taken. Reading it
		   here is what makes the index cheap: the rest of the record is not
		   touched until someone asks for that round. */
		double at = 0.0;

		if (std::fread(&at, 1, sizeof(at), opened) != sizeof(at))
			break;

		IndexEntry entry;
		entry.offset = static_cast<uint64_t>(payloadStart);
		entry.atSeconds = at;
		entry.payloadBytes = payloadBytes;
		scanned.push_back(entry);

		std::fseek(opened, payloadStart + static_cast<long>(payloadBytes), SEEK_SET);
	}

	/* A file whose first record cannot be read is not a recording as far as the
	   interface is concerned. One whose tail is cut is: the rounds that are
	   there are still a reading of the target. */
	index = std::move(scanned);

	if (index.empty())
	{
		std::fclose(opened);
		problem = Problem::NoRounds;

		return false;
	}

	/* The counter of a running recording must not be confused with this one:
	   both roles share the handle, and only one is active at a time. */
	{
		std::lock_guard<std::mutex> lock(mtx);
		file = opened;
		filePath = path;
		fileVersion = version;
		reading = true;
	}

	return true;
}

void RtosRecording::close()
{
	std::FILE* closing = nullptr;

	{
		std::lock_guard<std::mutex> lock(mtx);

		if (!reading && file == nullptr)
		{
			index.clear();

			return;
		}

		closing = file;
		file = nullptr;
		reading = false;
	}

	index.clear();

	if (closing != nullptr)
		std::fclose(closing);
}

double RtosRecording::atSeconds(size_t round) const
{
	if (round >= index.size())
		return 0.0;

	return index[round].atSeconds;
}

size_t RtosRecording::nearestRound(double atSeconds) const
{
	if (index.empty())
		return 0;

	if (!(atSeconds > index.front().atSeconds))
		return 0;

	if (atSeconds >= index.back().atSeconds)
		return index.size() - 1;

	/* The index is in time order, so the answer is one past the last round
	   that is earlier than the moment asked about; the nearer of the two is
	   the answer. */
	const auto after = std::upper_bound(index.begin(), index.end(), atSeconds, [](double value, const IndexEntry& entry)
	{
		return value < entry.atSeconds;
	});

	const size_t next = static_cast<size_t>(after - index.begin());

	if (next == 0)
		return 0;

	if (next >= index.size())
		return index.size() - 1;

	const size_t previous = next - 1;
	const double toPrevious = std::abs(atSeconds - index[previous].atSeconds);
	const double toNext = std::abs(index[next].atSeconds - atSeconds);

	return toNext < toPrevious ? next : previous;
}

std::shared_ptr<RtosSnapshot> RtosRecording::roundAt(size_t round) const
{
	return roundWithEvents(round).snapshot;
}

RtosRecording::Round RtosRecording::roundWithEvents(size_t round) const
{
	Round decoded;

	if (round >= index.size())
		return decoded;

	const IndexEntry& entry = index[round];

	/* A seek offset has to fit in the `long` the C library takes, which is
	   thirty-two bits here: two gigabytes is three hours of rounds, and a file
	   that big is refused rather than read from the wrong place. */
	if (entry.offset > static_cast<uint64_t>(LONG_MAX))
		return decoded;

	/* The read happens under the lock that `close` takes: a handle closed
	   while it is being read is a crash, and rounds are read at the speed of
	   someone clicking, not at the speed of the sampler. */
	std::lock_guard<std::mutex> lock(mtx);

	if (file == nullptr)
		return decoded;

	if (std::fseek(file, static_cast<long>(entry.offset), SEEK_SET) != 0)
		return decoded;

	std::vector<uint8_t> bytes(entry.payloadBytes);

	if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
		return decoded;

	Source source;
	source.bytes = bytes.data();
	source.size = bytes.size();

	std::shared_ptr<RtosSnapshot> snapshot = std::make_shared<RtosSnapshot>();

	snapshot->captureStartedAt = source.d();
	snapshot->captureFinishedAt = source.d();
	snapshot->captureSpanSeconds = source.d();
	snapshot->consistency = static_cast<RtosConsistency>(source.u8());
	snapshot->listsWalked = source.u32();
	snapshot->listsFailed = source.u32();

	snapshot->queueRegistryAvailable = source.u8() != 0;

	snapshot->stackScan.enabled = source.u8() != 0;
	snapshot->stackScan.fillSeen = source.u8() != 0;
	snapshot->stackScan.tasksScanned = source.u32();
	snapshot->stackScan.oldestReadingAgeSeconds = source.d();

	snapshot->cpuLoad.supported = source.u8() != 0;
	snapshot->cpuLoad.counting = source.u8() != 0;
	snapshot->cpuLoad.tasksEstimated = source.u32();
	snapshot->cpuLoad.tasksMeasured = source.u32();
	snapshot->cpuLoad.tasksKnown = source.u32();
	snapshot->cpuLoad.windowSeconds = source.d();

	snapshot->sessionId = source.text();

	const uint32_t problems = source.u32();

	for (uint32_t index = 0; index < problems && source.ok; index++)
		snapshot->problems.push_back(source.text());

	const uint32_t inconsistencies = source.u32();

	for (uint32_t index = 0; index < inconsistencies && source.ok; index++)
		snapshot->inconsistencies.push_back(source.text());

	const uint32_t layers = source.u32();

	for (uint32_t index = 0; index < layers && source.ok; index++)
	{
		RtosLayerAge age;
		age.layer = source.text();
		age.ageSeconds = source.d();
		snapshot->layerAges.push_back(age);
	}

	const uint32_t tasks = source.u32();

	/* A count that cannot fit in this record is a corrupt one; the loop stops
	   on `ok` and the round is refused below rather than filled with zeroes. */
	for (uint32_t index = 0; index < tasks && source.ok; index++)
		snapshot->tasks.push_back(readTask(source));

	const uint32_t objects = source.u32();

	for (uint32_t index = 0; index < objects && source.ok; index++)
		snapshot->syncObjects.push_back(readObject(source));

	/* Version 1 ends here; a version 2 record carries the runs as well. The
	   index was built before anyone knew which round would be read, so the
	   version is kept beside the handle rather than per record. */
	if (fileVersion >= 2)
	{
		const uint32_t runs = source.u32();

		for (uint32_t index = 0; index < runs && source.ok; index++)
		{
			RtosEventRing::Interval run;
			run.tcbAddress = source.u32();
			run.fromSeconds = source.d();
			run.toSeconds = source.d();

			decoded.events.push_back(run);
		}
	}

	if (!source.ok)
		return decoded;

	decoded.snapshot = std::move(snapshot);

	return decoded;
}

RtosRecording::Info RtosRecording::info() const
{
	Info result;
	result.rounds = index.size();

	if (!index.empty())
	{
		result.firstSecond = index.front().atSeconds;
		result.lastSecond = index.back().atSeconds;
	}

	std::lock_guard<std::mutex> lock(mtx);

	if (file != nullptr)
	{
		const long here = std::ftell(file);

		std::fseek(file, 0, SEEK_END);
		result.bytes = static_cast<uint64_t>(std::ftell(file));
		std::fseek(file, here, SEEK_SET);
	}

	return result;
}
