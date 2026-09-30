#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "DataHandlerBase.hpp"
#include "IDebugProbe.hpp"
#include "MovingAverage.hpp"
#include "VariableHandler.hpp"

class ViewerDataHandler : public DataHandlerBase
{
   public:
	static constexpr uint32_t minSamplinFrequencyHz = 1;
	static constexpr uint32_t maxSamplinFrequencyHz = 1000000;

	typedef struct Settings
	{
		uint32_t sampleFrequencyHz = 100;
		uint32_t maxPoints = 10000;
		uint32_t maxViewportPoints = 5000;
		bool refreshAddressesOnElfChange = false;
		bool stopAcqusitionOnElfChange = false;
		bool shouldLog = false;
		std::string logFilePath = "";
		std::string gdbCommand = "gdb";

		/* Which program reads the symbol file. A name rather than an object,
		   because the setting is stored in the project file and a project is
		   opened before the parser exists. The names are the ones the parser
		   interface answers to. */
		std::string elfParser = "gdb";
		std::string ofd2000Command = "ofd2000";
	} Settings;

	ViewerDataHandler(PlotGroupHandler* plotGroupHandler, VariableHandler* variableHandler, PlotHandler* plotHandler, PlotHandler* tracePlotHandler, std::atomic<bool>& done, std::mutex* mtx, spdlog::logger* logger);
	virtual ~ViewerDataHandler();

	std::string getLastReaderError() const;

	/* Writes a value to the target. The per variable range guard lives here
	   rather than in the caller, so that the plot table and the API are checked
	   the same way. Set the limits with Variable::setWriteLimits. */
	bool writeVariable(Variable& var, double value);

	/* Why the last writeVariable() call returned false. Empty when it did not,
	   and when the failure was the probe rather than a limit. */
	std::string getLastWriteError() const;

	/* The set of addresses read from the target is derived from the plots of the
	   active group and is only rebuilt when the state changes. Changing the group
	   or the plot membership while running therefore has to ask for a restart;
	   the acquisition thread performs the stop, rebuild and start in order, which
	   is what makes the sequence free of races. Ignored while stopped. */
	void requestRestart();

	IDebugProbe::DebugProbeSettings getProbeSettings() const;
	void setProbeSettings(const IDebugProbe::DebugProbeSettings& settings);

	void setDebugProbe(std::shared_ptr<IDebugProbe> probe);

	/* The open probe, so that a part of the application that moves memory outside
	   the sampling loop - the recorder - can reach the target through the same
	   connection instead of opening a second one. */
	std::shared_ptr<IDebugProbe> getDebugProbe() const
	{
		return debugProbe;
	}

	/* Guards the probe object. The sampling loop holds it around every call into
	   the probe, and a caller that moves memory outside the loop holds it for the
	   duration of its own transactions. Without it two threads would interleave
	   their frames on one connection and neither would receive the answer it
	   asked for. */
	std::unique_lock<std::mutex> lockProbe()
	{
		return std::unique_lock<std::mutex>(probeMutex);
	}

	Settings getSettings() const;
	void setSettings(const Settings& newSettings);

	double getAverageSamplingFrequency() const
	{
		if (averageSamplingPeriod > 0.0)
			return 1.0 / averageSamplingPeriod;
		return 0.0;
	}

   private:
	using SampleListType = std::vector<std::pair<uint32_t, uint8_t>>;

	void updateVariables(double timestamp, const std::unordered_map<uint32_t, double>& values);
	void dataHandler();
	void prepareCSVFile();
	void createSampleList();

   private:
	static constexpr size_t maxVariablesOnSinglePlot = 100;
	std::shared_ptr<IDebugProbe> debugProbe;
	IDebugProbe::DebugProbeSettings probeSettings{};
	std::atomic<bool> restartRequested{false};
	MovingAverage samplingPeriodFilter{1000};
	double averageSamplingPeriod = 0.0;
	Settings settings{};
	std::unordered_map<std::string, double> csvEntry;
	std::string lastWriteError;

	/* See lockProbe(). Guards the probe and everything reached through it. */
	std::mutex probeMutex;

	SampleListType sampleList;
};