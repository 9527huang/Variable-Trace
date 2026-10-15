#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

struct DisplayPoints
{
	std::vector<double> x;
	std::vector<double> y;
};

inline constexpr uint32_t displayDecimationMinimumSamplesPerPixel = 3;

inline bool shouldDecimateDisplaySeries(uint32_t count, uint32_t pixelColumns)
{
	return pixelColumns > 0 && static_cast<uint64_t>(count) >
		static_cast<uint64_t>(pixelColumns) * displayDecimationMinimumSamplesPerPixel;
}

inline void decimateDisplaySeries(const double* xValues, const double* yValues,
									 uint32_t count, uint32_t offset,
									 double xMin, double xMax, uint32_t pixelColumns,
									 DisplayPoints& result)
{
	result.x.clear();
	result.y.clear();
	if (xValues == nullptr || yValues == nullptr || count == 0 || pixelColumns == 0 || !(xMax > xMin))
		return;

	auto position = [count, offset](uint32_t logical) { return (logical + offset) % count; };
	auto boundary = [&](double value, bool upper)
	{
		uint32_t low = 0;
		uint32_t high = count;
		while (low < high)
		{
			const uint32_t middle = low + (high - low) / 2;
			const double candidate = xValues[position(middle)];
			if (candidate < value || (upper && candidate == value))
				low = middle + 1;
			else
				high = middle;
		}
		return low;
	};
	const uint32_t first = boundary(xMin, false);
	const uint32_t last = boundary(xMax, true);

	if (first == last)
		return;

	const uint32_t visible = last - first;
	if (static_cast<uint64_t>(visible) <= static_cast<uint64_t>(pixelColumns) * 2)
	{
		result.x.reserve(visible);
		result.y.reserve(visible);
		for (uint32_t logical = first; logical < last; ++logical)
		{
			const uint32_t slot = position(logical);
			result.x.push_back(xValues[slot]);
			result.y.push_back(yValues[slot]);
		}
		return;
	}

	const uint64_t outputCapacity = std::min<uint64_t>(visible, static_cast<uint64_t>(pixelColumns) * 2 + 2);
	result.x.reserve(static_cast<std::size_t>(outputCapacity));
	result.y.reserve(static_cast<std::size_t>(outputCapacity));
	auto append = [&](uint32_t logical)
	{
		const uint32_t slot = position(logical);
		if (!result.x.empty() && result.x.back() == xValues[slot] && result.y.back() == yValues[slot])
			return;
		result.x.push_back(xValues[slot]);
		result.y.push_back(yValues[slot]);
	};

	append(first);
	uint32_t binStart = first;
	for (uint32_t column = 0; column < pixelColumns && binStart < last; ++column)
	{
		const double right = column + 1 == pixelColumns ? xMax : xMin + (xMax - xMin) * (column + 1) / pixelColumns;
		uint32_t binEnd = binStart;
		while (binEnd < last && xValues[position(binEnd)] <= right)
			binEnd++;
		if (binEnd == binStart)
			continue;

		uint32_t minimum = binStart;
		uint32_t maximum = binStart;
		for (uint32_t logical = binStart + 1; logical < binEnd; ++logical)
		{
			if (yValues[position(logical)] < yValues[position(minimum)])
				minimum = logical;
			if (yValues[position(logical)] > yValues[position(maximum)])
				maximum = logical;
		}
		append(std::min(minimum, maximum));
		append(std::max(minimum, maximum));
		binStart = binEnd;
	}
	append(last - 1);
}

inline DisplayPoints decimateDisplaySeries(const double* xValues, const double* yValues,
											 uint32_t count, uint32_t offset,
											 double xMin, double xMax, uint32_t pixelColumns)
{
	DisplayPoints result;
	decimateDisplaySeries(xValues, yValues, count, offset, xMin, xMax, pixelColumns, result);
	return result;
}
