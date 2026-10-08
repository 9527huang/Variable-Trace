#ifndef __SCROLLINGBUFFER_HPP
#define __SCROLLINGBUFFER_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

/* A ring of samples that keeps the newest ones and drops the oldest as they
   are pushed out, read back in the order they arrived.

   How many it keeps is a setting the reader owns, so the two arrays behind it
   are allocated to the capacity in hand rather than sized into the type. A
   fixed ceiling would decide for the reader how long a run can be read back,
   and that is a question about how much memory this machine has to spare, not
   about how the ring is written.

   Both arrays carry one slot more than the capacity. The drawing and the
   statistics reach for the element one past the last sample, because that is
   how an end index is spelled, so the slot has to exist even though no sample
   is ever written to it. */
template <typename T>
class ScrollingBuffer
{
   public:
	ScrollingBuffer() = default;
	~ScrollingBuffer() = default;

	void addPoint(T x)
	{
		std::lock_guard<std::mutex> lock(mtx);

		if (maxSize == 0)
			return;

		data[offset] = x;
		offset = (offset + 1) % maxSize;

		if (offset == 0)
			isFull = true;
	}

	uint32_t getSize() const
	{
		std::lock_guard<std::mutex> lock(mtx);
		return sizeLocked();
	}

	T* getFirstElement() const
	{
		std::lock_guard<std::mutex> lock(mtx);
		return data.empty() ? nullptr : &data[0];
	}

	void copyData()
	{
		std::lock_guard<std::mutex> lock(mtx);

		if (maxSize == 0)
			return;

		std::memcpy(&dataCopy[0], &data[0], maxSize * sizeof(data[0]));
	}

	T* getFirstElementCopy() const
	{
		std::lock_guard<std::mutex> lock(mtx);
		return dataCopy.empty() ? nullptr : &dataCopy[0];
	}

	/* The newest sample, and the one that has been in hand the longest.

	   Neither is read straight from the write cursor: a cursor that has come
	   back round to the first slot still has the newest sample at the end of
	   the ring, not at the slot it is about to write. */
	T* getLastElement()
	{
		std::lock_guard<std::mutex> lock(mtx);

		if (data.empty() || sizeLocked() == 0)
			return nullptr;

		return &data[slotLocked(sizeLocked() - 1)];
	}

	T getNewestValue()
	{
		std::lock_guard<std::mutex> lock(mtx);

		if (sizeLocked() == 0)
			return T();

		return data[slotLocked(sizeLocked() - 1)];
	}

	T getOldestValue()
	{
		std::lock_guard<std::mutex> lock(mtx);

		if (sizeLocked() == 0)
			return T();

		return data[slotLocked(0)];
	}

	uint32_t getOffset() const
	{
		std::lock_guard<std::mutex> lock(mtx);
		return offset;
	}

	void erase()
	{
		std::lock_guard<std::mutex> lock(mtx);
		offset = 0;
		isFull = false;
	}

	/* Sets how many samples are kept.

	   What is in hand is the newest stretch of the run, so growing the ring
	   keeps all of it and shrinking keeps as many as still fit - what is given
	   up is the oldest, which is the end a running ring drops from anyway.
	   Asking for the capacity already in use does nothing, which is the
	   common case: the settings are applied on every frame the panel that
	   shows them is drawn. */
	void setMaxSize(uint32_t newMaxSize)
	{
		std::lock_guard<std::mutex> lock(mtx);

		if (newMaxSize == maxSize)
			return;

		const uint32_t size = sizeLocked();
		const uint32_t kept = std::min(size, newMaxSize);

		/* What does not fit is the oldest end, so the samples carried over
		   begin that far into the ring. */
		const uint32_t dropped = size - kept;

		std::vector<T> resized(newMaxSize + 1);
		std::vector<T> resizedCopy(newMaxSize + 1);

		for (uint32_t index = 0; index < kept; index++)
			resized[index] = data[slotLocked(dropped + index)];

		data.swap(resized);
		dataCopy.swap(resizedCopy);

		maxSize = newMaxSize;
		offset = newMaxSize == 0 ? 0 : kept % newMaxSize;
		isFull = newMaxSize != 0 && kept == newMaxSize;
	}

	uint32_t getMaxSize() const
	{
		return maxSize;
	}

	uint32_t getIndexFromvalue(double value)
	{
		for (uint32_t t = 0; t < getSize(); t++)
		{
			double first = data[t];

			if ((t + 1) >= getSize())
				return getSize() - 1;

			double second = data[t + 1];
			if (value >= first && value < second)
				return t;
		}
		return 0;
	}

	std::vector<double> getLinearData(size_t startIndex, size_t stopIndex)
	{
		std::vector<double> vec;

		if (getSize() == 0)
			return vec;

		if (startIndex < stopIndex)
			vec.insert(vec.end(), data.begin() + startIndex, data.begin() + stopIndex);
		else if (startIndex > stopIndex || (isFull && startIndex == stopIndex))
		{
			vec.insert(vec.end(), data.begin() + startIndex, data.begin() + getSize());
			vec.insert(vec.end(), data.begin(), data.begin() + stopIndex);
		}
		else if (startIndex == stopIndex)
			vec.insert(vec.end(), data.begin(), data.begin() + getSize());

		return vec;
	}

   private:
	/* How many samples are in hand and which slot each of them sits in, in the
	   order they arrived. The accessors above take these under the lock; the
	   two lookups further up work in slot order, as they always have. */
	uint32_t sizeLocked() const
	{
		return isFull ? maxSize : offset;
	}

	uint32_t slotLocked(uint32_t index) const
	{
		if (maxSize == 0)
			return 0;

		return isFull ? (index + offset) % maxSize : index;
	}

	mutable std::mutex mtx;

	/* Declared before the two arrays so that they start out allocated. */
	uint32_t maxSize = 10000;
	uint32_t offset = 0;
	bool isFull = false;

	mutable std::vector<T> data{std::vector<T>(maxSize + 1)};
	mutable std::vector<T> dataCopy{std::vector<T>(maxSize + 1)};
};

#endif
