#include "RtosIdentityStore.hpp"

namespace
{
	RtosEntityKey handleKey(const RtosEntityRef& ref, uint64_t epoch)
	{
		RtosEntityKey key;
		key.epoch = epoch;
		key.kind = ref.kind;
		key.rawHandle = ref.rawHandle;
		key.generation = 0;

		return key;
	}
}  // namespace

RtosIdentityStore::Outcome RtosIdentityStore::observe(const RtosEntityRef& ref, RtosTraceEventType type, uint64_t epoch)
{
	Outcome outcome;

	/* Nothing to key on. The event is kept by the caller, but it belongs to no
	   entity - which is a different statement from "it belongs to entity 0". */
	if (ref.kind == RtosEntityKind::None || ref.rawHandle == 0)
	{
		outcome.key.epoch = epoch;
		outcome.key.kind = ref.kind;

		return outcome;
	}

	const RtosEntityKey index = handleKey(ref, epoch);
	HandleState& state = handles[index];

	if (opensLifetime(type))
	{
		/* A creation for a handle that already has a lifetime open: the target
		   skipped a deletion, or two lifetimes were collapsed on the wire.
		   Close the old one rather than overwrite it, and say so. */
		if (state.open)
		{
			outcome.implicitClose = true;

			const RtosEntityKey previous = open(ref, epoch, state.openGeneration, false, false);
			entities[previous].closed = true;
		}

		const uint32_t generation = state.highestGeneration + 1;

		outcome.key = open(ref, epoch, generation, false, gapPending);
		outcome.afterGap = gapPending;
		gapPending = false;

		return outcome;
	}

	if (closesLifetime(type))
	{
		/* A deletion for a handle with no open lifetime: the creation was in a
		   gap, or the stream started after it. Record the closing anyway - a
		   tombstone with an unknown generation is still the fact that
		   something ended here. */
		if (!state.open)
		{
			outcome.key = open(ref, epoch, state.highestGeneration + 1, true, true);
			entities[outcome.key].closed = true;
			outcome.afterGap = true;
			gapPending = false;

			return outcome;
		}

		outcome.key = open(ref, epoch, state.openGeneration, false, false);
		entities[outcome.key].closed = true;
		outcome.afterGap = gapPending;
		gapPending = false;
		state.open = false;
		state.openGeneration = 0;

		return outcome;
	}

	/* An ordinary event. If a lifetime is open, it belongs to that one. */
	if (state.open)
	{
		outcome.key = open(ref, epoch, state.openGeneration, false, false);

		return outcome;
	}

	/* No lifetime open. Either nothing was ever announced for this handle, or
	   one ended earlier. Both mean the same thing for the identity: this is a
	   lifetime nobody opened for us, so its generation is a guess and its name
	   can only come from elsewhere. After a deletion the guess is sound - there
	   was a lifetime before, so this is a new one; on a fresh handle it is
	   honest to say the generation is unknown. */
	const uint32_t generation = state.highestGeneration + 1;
	const bool unknown = state.highestGeneration == 0;

	outcome.key = open(ref, epoch, generation, unknown, gapPending);
	outcome.openedWithoutCreation = true;

	return outcome;
}

RtosEntityKey RtosIdentityStore::open(const RtosEntityRef& ref, uint64_t epoch, uint32_t generation, bool generationUnknown, bool afterGap)
{
	const RtosEntityKey index = handleKey(ref, epoch);
	HandleState& state = handles[index];

	RtosEntityKey key = index;
	key.generation = generation;

	Entity& entity = entities[key];

	if (entity.key.rawHandle == 0 && entity.key.generation == 0)
	{
		entity.key = key;
		entity.generationUnknown = generationUnknown;
		entity.afterGap = entity.afterGap || afterGap;
	}

	/* An existing open entity keeps its flags; a lifecycle event that opens it
	   again does not unlearn that its generation is a guess. */
	if (generationUnknown)
		entity.generationUnknown = true;

	state.open = true;
	state.openGeneration = generation;

	if (generation > state.highestGeneration)
		state.highestGeneration = generation;

	return key;
}

void RtosIdentityStore::noteGap()
{
	gapPending = true;
}

const RtosIdentityStore::Entity* RtosIdentityStore::find(const RtosEntityKey& key) const
{
	const auto found = entities.find(key);

	return found == entities.end() ? nullptr : &found->second;
}

void RtosIdentityStore::nameFromSnapshot(const RtosEntityKey& key, const std::string& name)
{
	const auto found = entities.find(key);

	if (found == entities.end())
		return;

	/* A dictionary name is what the target said at that point in time and it
	   wins over anything a later snapshot claims: a snapshot describes the
	   present, and the present is not the history. */
	if (found->second.nameFromDictionary)
		return;

	found->second.name = name;
}

void RtosIdentityStore::nameFromDictionary(const RtosEntityKey& key, const std::string& name)
{
	const auto found = entities.find(key);

	if (found == entities.end())
		return;

	found->second.name = name;
	found->second.nameFromDictionary = true;
}

void RtosIdentityStore::clear()
{
	entities.clear();
	handles.clear();
	gapPending = false;
}

size_t RtosIdentityStore::openCount() const
{
	size_t count = 0;

	for (const auto& entry : entities)
	{
		if (!entry.second.closed)
			count++;
	}

	return count;
}

const RtosIdentityStore::Entity* RtosIdentityStore::openEntityFor(RtosEntityKind kind, uint32_t rawHandle) const
{
	for (const auto& entry : entities)
	{
		if (entry.second.closed)
			continue;

		if (entry.first.kind == kind && entry.first.rawHandle == rawHandle)
			return &entry.second;
	}

	return nullptr;
}
