#include "ViewerDataHandler.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>

#include "JlinkDebugProbe.hpp"
#include "StlinkDebugProbe.hpp"

ViewerDataHandler::ViewerDataHandler(PlotGroupHandler* plotGroupHandler, VariableHandler* variableHandler, PlotHandler* plotHandler, PlotHandler* tracePlotHandler, std::atomic<bool>& done, std::mutex* mtx, spdlog::logger* logger) : DataHandlerBase(plotGroupHandler, variableHandler, plotHandler, tracePlotHandler, done, mtx, logger, "viewer")
{
	dataHandle = std::thread(&ViewerDataHandler::dataHandler, this);
}
ViewerDataHandler::~ViewerDataHandler()
{
	if (dataHandle.joinable())
		dataHandle.join();
}

bool ViewerDataHandler::writeVariable(Variable& var, double value)
{
	lastWriteError.clear();

	/* The guard runs before the lock, because it reads nothing the sampling
	   thread writes. A variable without limits reports as allowed, which is the
	   behaviour the application had before limits existed. */
	if (!var.isWriteAllowed(value))
	{
		lastWriteError = "Value " + std::to_string(value) + " is out of write limits [" +
						 std::to_string(var.getWriteMin()) + ", " + std::to_string(var.getWriteMax()) + "]";
		return false;
	}

	std::lock_guard<std::mutex> lock(*mtx);
	uint32_t rawValue = var.getRawFromDouble(value);
	return debugProbe->writeMemory(var.getAddress(), (uint8_t*)&rawValue, var.getSize());
}

std::string ViewerDataHandler::getLastWriteError() const
{
	return lastWriteError;
}

std::string ViewerDataHandler::getLastReaderError() const
{
	return debugProbe->getLastErrorMsg();
}

bool ViewerDataHandler::isProbeHeld() const
{
	return debugProbe != nullptr && debugProbe->isValid();
}

void ViewerDataHandler::setDebugProbe(std::shared_ptr<IDebugProbe> probe)
{
	debugProbe = probe;
}

IDebugProbe::DebugProbeSettings ViewerDataHandler::getProbeSettings() const
{
	return probeSettings;
}

void ViewerDataHandler::setProbeSettings(const IDebugProbe::DebugProbeSettings& settings)
{
	probeSettings = settings;
}

ViewerDataHandler::Settings ViewerDataHandler::getSettings() const
{
	return settings;
}

void ViewerDataHandler::setSettings(const Settings& newSettings)
{
	settings = newSettings;

	/* What is kept here is what the buffers can hold. A capacity that does not
	   fit the memory the buffers are allowed is refused by the plot handler,
	   and the viewport is a fraction of that same capacity, so both are cut
	   down to it here rather than in each caller - a project file written on
	   a machine with more memory comes in through one of them. */
	settings.maxPoints = std::clamp(settings.maxPoints, static_cast<uint32_t>(1), plotHandler->maxPointsForMemoryBudget());
	settings.maxViewportPoints = std::clamp(settings.maxViewportPoints, static_cast<uint32_t>(1), settings.maxPoints);

	plotHandler->setMaxPoints(settings.maxPoints);
}

void ViewerDataHandler::requestRestart()
{
	restartRequested.store(true);
}

void ViewerDataHandler::updateVariables(double timestamp, const std::unordered_map<uint32_t, double>& values)
{
	/* The variables and the plots drawn from them are read as one model. The
	   reader can add, delete and rename while this is running, and each of
	   those is two changes to it - a variable and the series that name it -
	   which are made under the same lock this holds. */
	auto modelLock = variableHandler->lockModel();

	/* get raw values and put them into variables based on addresses */
	for (std::shared_ptr<Variable> var : *variableHandler)
	{
		uint32_t address = var->getAddress();
		if (values.contains(address))
			var->setRawValue(values.at(address));
	}

	for (std::shared_ptr<Variable> var : *variableHandler)
	{
		uint32_t address = var->getAddress();
		if (values.contains(address))
			csvEntry[var->getName()] = var->transformToDouble();
	}

	for (auto plot : *plotHandler)
	{
		std::lock_guard<std::mutex> lock(*mtx);
		/* thread-safe part */
		plot->updateSeries();
		plot->addTimePoint(timestamp);
	}

	if (settings.shouldLog)
		csvStreamer->writeLine(timestamp, csvEntry);
}

void ViewerDataHandler::dataHandler()
{
	std::chrono::time_point<std::chrono::steady_clock> start;
	uint32_t timer = 0;
	double lastT = 0.0;

	while (!done)
	{
		if (viewerState == State::RUN)
		{
			double period = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start).count();

			if (probeSettings.mode == IDebugProbe::Mode::HSS)
			{
				std::optional<IDebugProbe::varEntryType> maybeEntry;

				/* Held only around the probe call, so a caller that is waiting
				   for the connection gets it as soon as this read is done. */
				{
					std::unique_lock<std::mutex> probeLock = lockProbe();

					if (!debugProbe->isValid())
						setState(State::STOP);

					maybeEntry = debugProbe->readSingleEntry();
				}

				if (!maybeEntry.has_value())
					continue;

				auto [timestamp, rawValues] = maybeEntry.value();

				updateVariables(timestamp, rawValues);

				/* filter sampling frequency */
				averageSamplingPeriod = samplingPeriodFilter.filter((period - lastT));
				lastT = period;
				timer++;
			}

			else if (period > ((1.0 / settings.sampleFrequencyHz) * timer))
			{
				std::unordered_map<uint32_t, double> rawValues;

				/* One sample is a set of reads, and a caller that wants the
				   connection gets it between two samples rather than in the
				   middle of one. */
				{
					std::unique_lock<std::mutex> probeLock = lockProbe();

					/* sample by address */
					for (auto& [address, size] : sampleList)
					{
						uint32_t value = 0;
						if (debugProbe->readMemory(address, (uint8_t*)&value, size))
							rawValues[address] = value;
						else
							setState(State::STOP);
					}
				}

				double timestamp = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start).count();
				updateVariables(timestamp, rawValues);

				/* filter sampling frequency */
				averageSamplingPeriod = samplingPeriodFilter.filter((period - lastT));
				lastT = period;
				timer++;
			}
		}
		else
			std::this_thread::sleep_for(std::chrono::milliseconds(20));

		if (stateChangeOrdered)
		{
			if (viewerState == State::RUN)
			{
				createSampleList();
				prepareCSVFile();

				std::unique_lock<std::mutex> probeLock = lockProbe();

				if (debugProbe->startAcqusition(probeSettings, sampleList, settings.sampleFrequencyHz))
				{
					timer = 0;
					lastT = 0.0;
					start = std::chrono::steady_clock::now();
				}
				else
					viewerState = State::STOP;
			}
			else
			{
				std::unique_lock<std::mutex> probeLock = lockProbe();
				debugProbe->stopAcqusition();
				if (settings.shouldLog)
					csvStreamer->finishLogging();
			}
			stateChangeOrdered = false;
		}

		/* A restart builds the list of addresses read from the target again, which
		   is what makes a variable that was just added to a plot, or a newly
		   activated group, take effect.

		   It changes what is read, and the run carries on: the samples already on
		   the plots stay there, and what arrives now joins them at the end of the
		   time axis. Throwing them away is the start button's job, not this
		   one's. */
		if (restartRequested.exchange(false) && viewerState == State::RUN)
		{
			/* Built before anything else is decided, because how it came out is
			   what says whether there is anything to do at all. Importing a
			   variable that no plot draws leaves this list exactly as it was,
			   and it is the common case - a reader picks a symbol and puts it
			   in the table before deciding where to show it. */
			if (!createSampleList())
				continue;

			/* Names are logged by the plot they are drawn on, so a series that
			   has appeared or gone takes the header of the log with it. */
			prepareCSVFile();

			/* The addresses are configured on the probe's side in two cases
			   only: the streaming mode, where the probe builds the sample
			   stream out of the list it was handed, and the serial driver,
			   which sweeps a list this process sends it down the port. Those
			   have to be told again, which means putting them down and picking
			   them up.

			   Every other probe hands back a word at a time on request, and the
			   list those requests are made from is right here, so the next
			   sample reads the new one. Putting such a probe down would cost
			   every sample taken while it was down - a hole in each curve that
			   nothing asked for, and one the reader has no way to tell from a
			   target that stopped answering. */
			const bool theProbeReadsTheListItself = probeSettings.mode == IDebugProbe::Mode::HSS || probeSettings.debugProbe == IDebugProbe::Probe::Serial;

			if (!theProbeReadsTheListItself)
				continue;

			/* Taken before the probe is touched, so that the clock below is
			   moved along by the whole of the pause, the part spent starting it
			   again included. */
			const auto restartBegan = std::chrono::steady_clock::now();

			std::unique_lock<std::mutex> probeLock = lockProbe();
			debugProbe->stopAcqusition();

			const bool started = debugProbe->startAcqusition(probeSettings, sampleList, settings.sampleFrequencyHz);

			/* The clock keeps its place in the run. It is moved forward by the
			   time the restart takes, so that the stretch the target was not
			   read for does not show up in the history as a gap.

			   Putting it back to the beginning of the run instead would append
			   everything read from here on at the start of the time axis, and
			   since the samples of the run are still there, the curve would
			   fold back over the part already drawn.

			   The sample counter and the previous period are left where they
			   are along with it. Both of them measure the clock, so both still
			   agree with it; setting them back while the clock reads minutes
			   would let the rate limit pass every sample until they caught up
			   again, and the target would be read as fast as the probe allows
			   for that stretch. */
			start += std::chrono::steady_clock::now() - restartBegan;

			/* Same handling as a failing start from the state transition above. */
			if (!started)
				viewerState = State::STOP;
		}
	}
}

bool ViewerDataHandler::createSampleList()
{
	/* Walks the plots and the variables, so it takes the lock the interface
	   holds while it changes either of them. A variable added to a plot while
	   this is running is reached through requestRestart(), which runs this
	   again on this same thread. */
	auto modelLock = variableHandler->lockModel();

	/* Built beside the list in hand rather than over it, so that the two can be
	   compared before the one replaces the other. The answer is what the caller
	   decides on, and it has to be about the contents: the list carries the
	   addresses and the sizes of them, so a variable that keeps its address and
	   comes back a wider type counts as a change, as it should. */
	SampleListType wanted;

	auto checkIfElementExists = [&](std::pair<uint32_t, uint8_t> newElement)
	{
		return std::find_if(wanted.begin(), wanted.end(), [&newElement](const std::pair<uint32_t, uint8_t>& element)
							{ return element == newElement; }) != wanted.end();
	};

	for (auto& [name, plotElem] : *plotGroupHandler->getActiveGroup())
	{
		auto plot = plotElem.plot;

		if (!plotElem.visibility)
			continue;

		for (auto& [name, ser] : plot->getSeriesMap())
		{
			if (!ser->visible)
				continue;

			std::pair<uint32_t, uint8_t> newElement = {ser->var->getAddress(), ser->var->getSize()};

			if (!checkIfElementExists(newElement))
				wanted.push_back(newElement);

			Variable* maybeXAxisVariable = plot->getXAxisVariable();
			if (plot->getType() == Plot::Type::XY && maybeXAxisVariable != nullptr)
			{
				newElement = {maybeXAxisVariable->getAddress(), maybeXAxisVariable->getSize()};
				if (!checkIfElementExists(newElement))
					wanted.push_back(newElement);
			}
		}
	}

	/* additionally scan for eventual bases of fractional variables that should be sampled */
	for (auto variable : *variableHandler)
	{
		if (variable->getFractional().baseVariable != nullptr)
		{
			auto var = variable->getFractional().baseVariable;
			std::pair<uint32_t, uint8_t> newElement = {var->getAddress(), var->getSize()};

			if (!checkIfElementExists(newElement))
				wanted.push_back(newElement);
		}
	}

	const bool changed = wanted != sampleList;

	sampleList = std::move(wanted);

	/* mark actively sampled variables */
	for (auto variable : *variableHandler)
	{
		variable->setIsCurrentlySampled(false);
		if (std::find(sampleList.begin(), sampleList.end(), std::pair<uint32_t, uint8_t>(variable->getAddress(), variable->getSize())) != sampleList.end())
			variable->setIsCurrentlySampled(true);
	}

	return changed;
}

void ViewerDataHandler::prepareCSVFile()
{
	if (!settings.shouldLog)
		return;

	std::vector<std::string> headerNames;

	for (auto& [name, plotElem] : *plotGroupHandler->getActiveGroup())
	{
		auto plot = plotElem.plot;

		if (!plotElem.visibility)
			continue;

		for (auto& [name, ser] : plot->getSeriesMap())
			headerNames.push_back(name);
	}
	csvStreamer->prepareFile(settings.logFilePath);
	csvStreamer->createHeader(headerNames);
}