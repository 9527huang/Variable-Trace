#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "GlobalConfig.hpp"
#include "PlotHandler.hpp"
#include "PlotGroupHandler.hpp"
#include "ProjectHandler.hpp"
#include "VariableHandler.hpp"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/spdlog.h"

namespace
{
	/*
	 * Both handlers log while they work. A null sink keeps the test output clean
	 * and, more importantly, keeps the tests independent from the application
	 * logger, which is created in main().
	 */
	std::shared_ptr<spdlog::logger> makeSilentLogger()
	{
		return std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::null_sink_mt>());
	}

	class ProjectHandlerTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			logger = makeSilentLogger();
			directory = std::filesystem::temp_directory_path() / "variable-trace-project-test";
			std::filesystem::create_directories(directory);
		}

		void TearDown() override
		{
			std::error_code errorCode;
			std::filesystem::remove_all(directory, errorCode);
		}

		std::string path(const std::string& fileName) const
		{
			return (directory / fileName).string();
		}

		std::shared_ptr<spdlog::logger> logger;
		std::filesystem::path directory;
	};

	void writeFile(const std::string& filePath, const std::string& content)
	{
		std::ofstream file(filePath, std::ios::trunc);
		file << content;
	}
}  // namespace

TEST_F(ProjectHandlerTest, roundTripPreservesElfAndAcquisitionSettings)
{
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler handler(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	ProjectData data;
	data.elfPath = "firmware/target.elf";
	data.viewer.sampleFrequencyHz = 250;
	data.viewer.maxPoints = 12345;
	data.viewer.maxViewportPoints = 678;
	data.viewer.refreshAddressesOnElfChange = true;
	data.viewer.stopAcquisitionOnElfChange = true;
	data.viewer.loggingEnabled = true;
	data.viewer.logDirectory = "logs/viewer";
	data.viewer.gdbCommand = "arm-none-eabi-gdb";
	data.viewer.elfParser = "c2000";
	data.viewer.ofd2000Command = "C:/ti/ccs/tools/compiler/ti-cgt-c2000/ofd2000.exe";
	data.viewer.probe = {1, "SERIAL-VIEWER", "STM32F103C8", 1, 4000};

	data.trace.coreFrequency = 72000000;
	data.trace.tracePrescaler = 4;
	data.trace.maxPoints = 555;
	data.trace.maxViewportPointsPercent = 25;
	data.trace.triggerChannel = 3;
	data.trace.triggerLevel = 0.25;
	data.trace.shouldReset = true;
	data.trace.timeout = 7;
	data.trace.loggingEnabled = true;
	data.trace.logDirectory = "logs/trace";
	data.trace.probe = {0, "SERIAL-TRACE", "STM32F407VG", 0, 2000};

	const std::string file = path("settings.mcvproj");
	ASSERT_TRUE(handler.save(file, data));

	VariableHandler loadedVariables;
	PlotHandler loadedPlots;
	PlotGroupHandler loadedGroups;
	ProjectHandler reader(&loadedVariables, &loadedPlots, &loadedGroups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(file, loaded), ProjectHandler::OpenResult::Ok);

	EXPECT_EQ(loaded.elfPath, data.elfPath);

	EXPECT_EQ(loaded.viewer.sampleFrequencyHz, 250u);
	EXPECT_EQ(loaded.viewer.maxPoints, 12345u);
	EXPECT_EQ(loaded.viewer.maxViewportPoints, 678u);
	EXPECT_TRUE(loaded.viewer.refreshAddressesOnElfChange);
	EXPECT_TRUE(loaded.viewer.stopAcquisitionOnElfChange);
	EXPECT_TRUE(loaded.viewer.loggingEnabled);
	EXPECT_EQ(loaded.viewer.logDirectory, "logs/viewer");
	EXPECT_EQ(loaded.viewer.gdbCommand, "arm-none-eabi-gdb");
	EXPECT_EQ(loaded.viewer.elfParser, "c2000");
	EXPECT_EQ(loaded.viewer.ofd2000Command, "C:/ti/ccs/tools/compiler/ti-cgt-c2000/ofd2000.exe");
	EXPECT_EQ(loaded.viewer.probe.type, 1u);
	EXPECT_EQ(loaded.viewer.probe.serialNumber, "SERIAL-VIEWER");
	EXPECT_EQ(loaded.viewer.probe.targetName, "STM32F103C8");
	EXPECT_EQ(loaded.viewer.probe.mode, 1u);
	EXPECT_EQ(loaded.viewer.probe.speedKHz, 4000u);

	EXPECT_EQ(loaded.trace.coreFrequency, 72000000u);
	EXPECT_EQ(loaded.trace.tracePrescaler, 4u);
	EXPECT_EQ(loaded.trace.maxPoints, 555u);
	EXPECT_EQ(loaded.trace.maxViewportPointsPercent, 25u);
	EXPECT_EQ(loaded.trace.triggerChannel, 3);
	EXPECT_DOUBLE_EQ(loaded.trace.triggerLevel, 0.25);
	EXPECT_TRUE(loaded.trace.shouldReset);
	EXPECT_EQ(loaded.trace.timeout, 7u);
	EXPECT_TRUE(loaded.trace.loggingEnabled);
	EXPECT_EQ(loaded.trace.logDirectory, "logs/trace");
	EXPECT_EQ(loaded.trace.probe.type, 0u);
	EXPECT_EQ(loaded.trace.probe.serialNumber, "SERIAL-TRACE");
	EXPECT_EQ(loaded.trace.probe.targetName, "STM32F407VG");
	EXPECT_EQ(loaded.trace.probe.speedKHz, 2000u);
}

TEST_F(ProjectHandlerTest, aProjectWrittenBeforeTheParserSettingKeepsTheDefault)
{
	/* Which program reads the symbol file was added to the format later, so a
	   project saved by the previous version does not carry it. A field that is
	   missing has to come back as the default rather than as nothing, or the
	   project would open with no parser able to read its symbol file. */
	writeFile(path("older.mcvproj"), R"({"formatVersion": 1, "elf": {"path": "old.elf"}, )"
									  R"("acquisition": {"sampleFrequencyHz": 100, "gdbCommand": "gdb"}})");

	VariableHandler variables;
	PlotHandler plots;
	PlotGroupHandler groups;
	ProjectHandler reader(&variables, &plots, &groups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(path("older.mcvproj"), loaded), ProjectHandler::OpenResult::Ok);

	EXPECT_EQ(loaded.elfPath, "old.elf");
	EXPECT_EQ(loaded.viewer.elfParser, "gdb");
	EXPECT_EQ(loaded.viewer.ofd2000Command, "ofd2000");
}

TEST_F(ProjectHandlerTest, roundTripPreservesTracePlots)
{
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler handler(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	ProjectData data;
	data.tracePlots.push_back({"CH0", "channel zero", false, 1, 6, 0xFF00FF00});
	data.tracePlots.push_back({"CH1", "CH1", true, 0, 3, 0xFF123456});

	const std::string file = path("trace-plots.mcvproj");
	ASSERT_TRUE(handler.save(file, data));

	VariableHandler loadedVariables;
	PlotHandler loadedPlots;
	PlotGroupHandler loadedGroups;
	ProjectHandler reader(&loadedVariables, &loadedPlots, &loadedGroups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(file, loaded), ProjectHandler::OpenResult::Ok);

	ASSERT_EQ(loaded.tracePlots.size(), 2u);

	EXPECT_EQ(loaded.tracePlots[0].name, "CH0");
	EXPECT_EQ(loaded.tracePlots[0].alias, "channel zero");
	EXPECT_FALSE(loaded.tracePlots[0].visibility);
	EXPECT_EQ(loaded.tracePlots[0].domain, 1u);
	EXPECT_EQ(loaded.tracePlots[0].varType, 6u);
	EXPECT_EQ(loaded.tracePlots[0].color, 0xFF00FF00u);

	EXPECT_EQ(loaded.tracePlots[1].name, "CH1");
	EXPECT_TRUE(loaded.tracePlots[1].visibility);
	EXPECT_EQ(loaded.tracePlots[1].domain, 0u);
	EXPECT_EQ(loaded.tracePlots[1].varType, 3u);
	EXPECT_EQ(loaded.tracePlots[1].color, 0xFF123456u);
}

TEST_F(ProjectHandlerTest, roundTripPreservesWritePlans)
{
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler handler(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	ProjectWritePlan ramp{};
	ramp.name = "startup ramp";
	ramp.variable = "duty";
	ramp.steps.push_back({0.0, 0.0});
	ramp.steps.push_back({1.5, 50.0});
	ramp.steps.push_back({3.0, 100.0});

	ProjectWritePlan empty{};
	empty.name = "spare";
	empty.variable = "";

	ProjectData data;
	data.writePlans.push_back(ramp);
	data.writePlans.push_back(empty);

	const std::string file = path("write-plans.mcvproj");
	ASSERT_TRUE(handler.save(file, data));

	VariableHandler loadedVariables;
	PlotHandler loadedPlots;
	PlotGroupHandler loadedGroups;
	ProjectHandler reader(&loadedVariables, &loadedPlots, &loadedGroups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(file, loaded), ProjectHandler::OpenResult::Ok);

	ASSERT_EQ(loaded.writePlans.size(), 2u);

	EXPECT_EQ(loaded.writePlans[0].name, "startup ramp");
	EXPECT_EQ(loaded.writePlans[0].variable, "duty");
	ASSERT_EQ(loaded.writePlans[0].steps.size(), 3u);
	EXPECT_DOUBLE_EQ(loaded.writePlans[0].steps[0].time, 0.0);
	EXPECT_DOUBLE_EQ(loaded.writePlans[0].steps[1].value, 50.0);
	EXPECT_DOUBLE_EQ(loaded.writePlans[0].steps[2].time, 3.0);
	EXPECT_DOUBLE_EQ(loaded.writePlans[0].steps[2].value, 100.0);

	/* A plan with no steps is a plan the user has just created, so it has to
	   survive the file as well. */
	EXPECT_EQ(loaded.writePlans[1].name, "spare");
	EXPECT_TRUE(loaded.writePlans[1].steps.empty());
}

TEST_F(ProjectHandlerTest, roundTripKeepsWriteLimitsAndEnumLabels)
{
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler handler(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	variableHandler.addNewVariable("duty");
	auto duty = variableHandler.getVariable("duty");
	duty->setWriteLimits(true, 0.0, 100.0);

	variableHandler.addNewVariable("state");
	auto state = variableHandler.getVariable("state");
	state->setType(Variable::Type::U8);
	state->setHighLevelType(Variable::HighLevelType::CUSTOM_ENUM);
	state->setEnumLabels({{"idle", 0}, {"spinning", 1}, {"fault", 2}});

	variableHandler.addNewVariable("plain");
	variableHandler.getVariable("plain")->setWriteLimits(false, -5.0, 5.0);

	const std::string file = path("limits-and-enums.mcvproj");
	ASSERT_TRUE(handler.save(file, ProjectData{}));

	VariableHandler loadedVariables;
	PlotHandler loadedPlots;
	PlotGroupHandler loadedGroups;
	ProjectHandler reader(&loadedVariables, &loadedPlots, &loadedGroups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(file, loaded), ProjectHandler::OpenResult::Ok);

	ASSERT_TRUE(loadedVariables.contains("duty"));
	EXPECT_TRUE(loadedVariables.getVariable("duty")->getWriteLimitsEnabled());
	EXPECT_DOUBLE_EQ(loadedVariables.getVariable("duty")->getWriteMin(), 0.0);
	EXPECT_DOUBLE_EQ(loadedVariables.getVariable("duty")->getWriteMax(), 100.0);
	EXPECT_TRUE(loadedVariables.getVariable("duty")->isWriteAllowed(100.0));
	EXPECT_FALSE(loadedVariables.getVariable("duty")->isWriteAllowed(100.5));

	/* A range that is stored but switched off has to come back switched off, or
	   a project would silently start refusing writes after a reload. */
	EXPECT_FALSE(loadedVariables.getVariable("plain")->getWriteLimitsEnabled());
	EXPECT_DOUBLE_EQ(loadedVariables.getVariable("plain")->getWriteMin(), -5.0);

	ASSERT_TRUE(loadedVariables.contains("state"));
	EXPECT_TRUE(loadedVariables.getVariable("state")->isEnum());

	const std::vector<Variable::EnumLabel> labels = loadedVariables.getVariable("state")->getEnumLabels();
	ASSERT_EQ(labels.size(), 3u);
	EXPECT_EQ(labels[0].label, "idle");
	EXPECT_EQ(labels[1].value, 1);
	EXPECT_EQ(labels[2].label, "fault");
	EXPECT_EQ(loadedVariables.getVariable("state")->getEnumLabel(2.0), "fault");
	EXPECT_EQ(loadedVariables.getVariable("state")->getEnumLabel(9.0), "");
}

TEST_F(ProjectHandlerTest, aProjectWithoutWritePlansLoadsWithNone)
{
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler handler(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	ProjectData data;
	const std::string file = path("no-plans.mcvproj");
	ASSERT_TRUE(handler.save(file, data));

	VariableHandler loadedVariables;
	PlotHandler loadedPlots;
	PlotGroupHandler loadedGroups;
	ProjectHandler reader(&loadedVariables, &loadedPlots, &loadedGroups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(file, loaded), ProjectHandler::OpenResult::Ok);
	EXPECT_TRUE(loaded.writePlans.empty());
}

TEST_F(ProjectHandlerTest, aWritePlanWithoutANameIsSkipped)
{
	const std::string file = path("unnamed-plan.mcvproj");

	/* Written by hand: a name is what identifies a plan in the list, so one
	   without it could never be selected or removed. */
	writeFile(file,
			  R"({"formatVersion": 3, "writePlans": [)"
			  R"({"name": "", "variable": "duty", "steps": [{"time": 0, "value": 1}]},)"
			  R"({"name": "kept", "variable": "duty", "steps": [{"time": 0, "value": 2}]})"
			  R"(]})");

	VariableHandler loadedVariables;
	PlotHandler loadedPlots;
	PlotGroupHandler loadedGroups;
	ProjectHandler reader(&loadedVariables, &loadedPlots, &loadedGroups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(file, loaded), ProjectHandler::OpenResult::Ok);

	ASSERT_EQ(loaded.writePlans.size(), 1u);
	EXPECT_EQ(loaded.writePlans[0].name, "kept");
	EXPECT_DOUBLE_EQ(loaded.writePlans[0].steps[0].value, 2.0);
}

TEST_F(ProjectHandlerTest, roundTripRebuildsVariablesPlotsAndGroups)
{
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler handler(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	variableHandler.addNewVariable("speed");
	auto speed = variableHandler.getVariable("speed");
	speed->setAddress(0x20000000);
	speed->setType(Variable::Type::F32);
	speed->setColor(0.25f, 0.5f, 0.75f, 1.0f);
	speed->setShift(3);
	speed->setMask(0x00FF);
	speed->setHighLevelType(Variable::HighLevelType::SIGNEDFRAC);
	speed->setShouldUpdateFromElf(false);

	variableHandler.addNewVariable("torque");
	auto torque = variableHandler.getVariable("torque");
	torque->setTrackedName("motor.torque");
	torque->setIsTrackedNameDifferent(true);
	torque->setAddress(0x20000004);
	torque->setType(Variable::Type::I16);

	auto plot = plotHandler.addPlot("main");
	plot->setType(Plot::Type::BAR);
	plot->addSeries(speed.get());
	plot->getSeries("speed")->visible = false;
	plot->setSeriesDisplayFormat("speed", Plot::displayFormat::HEX);
	plot->addSeries(torque.get());

	auto group = plotGroupHandler.addGroup("group a");
	group->addPlot(plot, false);
	plotGroupHandler.setActiveGroup("group a");

	ProjectData data;
	data.elfPath = "target.elf";

	const std::string file = path("model.mcvproj");
	ASSERT_TRUE(handler.save(file, data));

	VariableHandler loadedVariables;
	PlotHandler loadedPlots;
	PlotGroupHandler loadedGroups;
	ProjectHandler reader(&loadedVariables, &loadedPlots, &loadedGroups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(file, loaded), ProjectHandler::OpenResult::Ok);

	ASSERT_TRUE(loadedVariables.contains("speed"));
	EXPECT_EQ(loadedVariables.getVariable("speed")->getAddress(), 0x20000000u);
	EXPECT_EQ(loadedVariables.getVariable("speed")->getType(), Variable::Type::F32);
	EXPECT_FLOAT_EQ(loadedVariables.getVariable("speed")->getColor().r, 0.25f);
	EXPECT_FLOAT_EQ(loadedVariables.getVariable("speed")->getColor().g, 0.5f);
	EXPECT_FLOAT_EQ(loadedVariables.getVariable("speed")->getColor().b, 0.75f);
	EXPECT_EQ(loadedVariables.getVariable("speed")->getShift(), 3u);
	EXPECT_EQ(loadedVariables.getVariable("speed")->getMask(), 0x00FFu);
	EXPECT_EQ(loadedVariables.getVariable("speed")->getHighLevelType(), Variable::HighLevelType::SIGNEDFRAC);
	EXPECT_FALSE(loadedVariables.getVariable("speed")->getShouldUpdateFromElf());

	ASSERT_TRUE(loadedVariables.contains("torque"));
	EXPECT_EQ(loadedVariables.getVariable("torque")->getTrackedName(), "motor.torque");
	EXPECT_TRUE(loadedVariables.getVariable("torque")->getIsTrackedNameDifferent());
	EXPECT_EQ(loadedVariables.getVariable("torque")->getType(), Variable::Type::I16);

	ASSERT_TRUE(loadedPlots.checkIfPlotExists("main"));
	auto loadedPlot = loadedPlots.getPlot("main");
	EXPECT_EQ(loadedPlot->getType(), Plot::Type::BAR);
	EXPECT_EQ(loadedPlot->getSeriesMap().size(), 2u);
	EXPECT_FALSE(loadedPlot->getSeries("speed")->visible);
	EXPECT_EQ(loadedPlot->getSeries("speed")->format, Plot::displayFormat::HEX);
	EXPECT_TRUE(loadedPlot->getSeries("torque")->visible);

	EXPECT_EQ(loadedGroups.getActiveGroupName(), "group a");
	EXPECT_FALSE(loadedGroups.getGroup("group a")->getVisibility("main"));
}

TEST_F(ProjectHandlerTest, roundTripKeepsPlotAxisLabels)
{
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler handler(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	auto labelled = plotHandler.addPlot("labelled");
	labelled->setXAxisLabel("elapsed [ms]");
	labelled->setYAxisLabel("angle [deg]");

	/* The usual state by far: nothing typed in, so each axis keeps the name
	   its kind gives it. That has to survive a save as well, which it does by
	   staying empty. */
	auto plain = plotHandler.addPlot("plain");

	ProjectData data;
	const std::string file = path("labels.mcvproj");
	ASSERT_TRUE(handler.save(file, data));

	VariableHandler loadedVariables;
	PlotHandler loadedPlots;
	PlotGroupHandler loadedGroups;
	ProjectHandler reader(&loadedVariables, &loadedPlots, &loadedGroups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(file, loaded), ProjectHandler::OpenResult::Ok);

	ASSERT_TRUE(loadedPlots.checkIfPlotExists("labelled"));
	EXPECT_EQ(loadedPlots.getPlot("labelled")->getXAxisLabel(), "elapsed [ms]");
	EXPECT_EQ(loadedPlots.getPlot("labelled")->getYAxisLabel(), "angle [deg]");
	EXPECT_EQ(loadedPlots.getPlot("labelled")->getEffectiveXAxisLabel(), "elapsed [ms]");

	ASSERT_TRUE(loadedPlots.checkIfPlotExists("plain"));
	EXPECT_EQ(loadedPlots.getPlot("plain")->getXAxisLabel(), "");
	EXPECT_EQ(loadedPlots.getPlot("plain")->getYAxisLabel(), "");
	EXPECT_EQ(loadedPlots.getPlot("plain")->getEffectiveXAxisLabel(), "time[s]");
}

TEST_F(ProjectHandlerTest, aPlotWrittenBeforeLabelsExistedKeepsTheAutomaticOnes)
{
	/* Version 3 files carry no label keys at all. A missing label has to read
	   back as an empty one, which stands for "no label of its own", or every
	   project saved before the labels were added would open with both axes
	   unnamed. */
	writeFile(path("no-labels.mcvproj"),
			  R"({"formatVersion": 3, "plots": [)"
			  R"({"name": "old", "type": 0, "series": []})"
			  R"(]})");

	VariableHandler variables;
	PlotHandler plots;
	PlotGroupHandler groups;
	ProjectHandler reader(&variables, &plots, &groups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(path("no-labels.mcvproj"), loaded), ProjectHandler::OpenResult::Ok);

	ASSERT_TRUE(plots.checkIfPlotExists("old"));
	EXPECT_EQ(plots.getPlot("old")->getXAxisLabel(), "");
	EXPECT_EQ(plots.getPlot("old")->getYAxisLabel(), "");
	EXPECT_EQ(plots.getPlot("old")->getEffectiveXAxisLabel(), "time[s]");
}

TEST_F(ProjectHandlerTest, roundTripKeepsTheCursors)
{
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler handler(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	auto measured = plotHandler.addPlot("measured");
	measured->setCursorsVisible(true);
	measured->setCursorMode(Plot::CursorMode::XY);
	measured->markerX0.setValue(1.5);
	measured->markerX1.setValue(2.5);
	measured->markerY0.setValue(-3.5);
	measured->markerY1.setValue(4.5);

	/* A plot that was never measured with is the usual case, and it has to
	   come back without cursors rather than with a default pair. */
	auto untouched = plotHandler.addPlot("untouched");

	ProjectData data;
	const std::string file = path("cursors.mcvproj");
	ASSERT_TRUE(handler.save(file, data));

	VariableHandler loadedVariables;
	PlotHandler loadedPlots;
	PlotGroupHandler loadedGroups;
	ProjectHandler reader(&loadedVariables, &loadedPlots, &loadedGroups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(file, loaded), ProjectHandler::OpenResult::Ok);

	ASSERT_TRUE(loadedPlots.checkIfPlotExists("measured"));
	const auto restored = loadedPlots.getPlot("measured");
	EXPECT_TRUE(restored->getCursorsVisible());
	EXPECT_EQ(restored->getCursorMode(), Plot::CursorMode::XY);
	EXPECT_EQ(restored->markerX0.getValue(), 1.5);
	EXPECT_EQ(restored->markerX1.getValue(), 2.5);
	EXPECT_EQ(restored->markerY0.getValue(), -3.5);
	EXPECT_EQ(restored->markerY1.getValue(), 4.5);

	ASSERT_TRUE(loadedPlots.checkIfPlotExists("untouched"));
	EXPECT_FALSE(loadedPlots.getPlot("untouched")->getCursorsVisible());
	EXPECT_EQ(loadedPlots.getPlot("untouched")->getCursorMode(), Plot::CursorMode::X);
}

TEST_F(ProjectHandlerTest, aPlotWrittenBeforeCursorsExistedOpensUnmeasured)
{
	/* Version 4 files carry no cursor keys, so a missing switch reads back as
	   cursors that are off - the same as a new plot. */
	writeFile(path("no-cursors.mcvproj"),
			  R"({"formatVersion": 4, "plots": [)"
			  R"({"name": "old", "type": 0, "xAxisLabel": "", "yAxisLabel": "", "series": []})"
			  R"(]})");

	VariableHandler variables;
	PlotHandler plots;
	PlotGroupHandler groups;
	ProjectHandler reader(&variables, &plots, &groups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(path("no-cursors.mcvproj"), loaded), ProjectHandler::OpenResult::Ok);

	ASSERT_TRUE(plots.checkIfPlotExists("old"));
	EXPECT_FALSE(plots.getPlot("old")->getCursorsVisible());
	EXPECT_EQ(plots.getPlot("old")->getCursorMode(), Plot::CursorMode::X);
}

TEST_F(ProjectHandlerTest, aCursorModeOutOfRangeFallsBackToTheFirst)
{
	/* The number comes out of a file that may have been written by a newer
	   build or edited by hand, so it is not trusted past the last mode. */
	writeFile(path("bad-mode.mcvproj"),
			  R"({"formatVersion": 5, "plots": [)"
			  R"({"name": "p", "type": 0, "cursorsVisible": true, "cursorMode": 77, "series": []})"
			  R"(]})");

	VariableHandler variables;
	PlotHandler plots;
	PlotGroupHandler groups;
	ProjectHandler reader(&variables, &plots, &groups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(path("bad-mode.mcvproj"), loaded), ProjectHandler::OpenResult::Ok);

	ASSERT_TRUE(plots.checkIfPlotExists("p"));
	EXPECT_EQ(plots.getPlot("p")->getCursorMode(), Plot::CursorMode::X);
}

TEST_F(ProjectHandlerTest, fractionalVariableKeepsItsBaseReference)
{
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler handler(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	variableHandler.addNewVariable("raw");
	variableHandler.addNewVariable("scaled");

	auto scaled = variableHandler.getVariable("scaled");

	/* isFractional() is driven by the high level type, so a fractional variable
	   always carries one of the two fractional kinds. */
	scaled->setHighLevelType(Variable::HighLevelType::SIGNEDFRAC);

	Variable::Fractional fractional;
	fractional.fractionalBits = 12;
	fractional.base = 2.5;
	fractional.baseVariable = variableHandler.getVariable("raw").get();
	scaled->setFractional(fractional);

	ASSERT_TRUE(handler.save(path("fractional.mcvproj"), ProjectData{}));

	VariableHandler loadedVariables;
	PlotHandler loadedPlots;
	PlotGroupHandler loadedGroups;
	ProjectHandler reader(&loadedVariables, &loadedPlots, &loadedGroups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(path("fractional.mcvproj"), loaded), ProjectHandler::OpenResult::Ok);

	ASSERT_TRUE(loadedVariables.contains("scaled"));
	EXPECT_TRUE(loadedVariables.getVariable("scaled")->isFractional());
	EXPECT_EQ(loadedVariables.getVariable("scaled")->getFractional().fractionalBits, 12u);
	EXPECT_DOUBLE_EQ(loadedVariables.getVariable("scaled")->getFractional().base, 2.5);
	ASSERT_NE(loadedVariables.getVariable("scaled")->getFractional().baseVariable, nullptr);
	EXPECT_EQ(loadedVariables.getVariable("scaled")->getFractional().baseVariable->getName(), "raw");
}

TEST_F(ProjectHandlerTest, projectWithoutGroupsGetsADefaultOne)
{
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler handler(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	plotHandler.addPlot("orphan");

	ASSERT_TRUE(handler.save(path("no-groups.mcvproj"), ProjectData{}));

	VariableHandler loadedVariables;
	PlotHandler loadedPlots;
	PlotGroupHandler loadedGroups;
	ProjectHandler reader(&loadedVariables, &loadedPlots, &loadedGroups, logger.get());

	ProjectData loaded;
	ASSERT_EQ(reader.open(path("no-groups.mcvproj"), loaded), ProjectHandler::OpenResult::Ok);

	ASSERT_EQ(loadedGroups.getGroupCount(), 1u);
	EXPECT_EQ(loadedGroups.getActiveGroupName(), "default group");
	EXPECT_TRUE(loadedGroups.getGroup("default group")->getVisibility("orphan"));
}

TEST_F(ProjectHandlerTest, newerFormatVersionIsRejected)
{
	writeFile(path("future.mcvproj"), R"({"formatVersion": 99, "application": "Variable-Trace"})");

	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler reader(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	ProjectData loaded;
	EXPECT_EQ(reader.open(path("future.mcvproj"), loaded), ProjectHandler::OpenResult::NewerFormatVersion);
}

TEST_F(ProjectHandlerTest, malformedFileIsReported)
{
	writeFile(path("broken.mcvproj"), "{ this is not json");

	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler reader(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	ProjectData loaded;
	EXPECT_EQ(reader.open(path("broken.mcvproj"), loaded), ProjectHandler::OpenResult::ParseError);
}

TEST_F(ProjectHandlerTest, missingFileIsReported)
{
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler reader(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	ProjectData loaded;
	EXPECT_EQ(reader.open(path("does-not-exist.mcvproj"), loaded), ProjectHandler::OpenResult::CannotOpenFile);
}

TEST_F(ProjectHandlerTest, jsonProjectIsToldApartFromLegacyConfig)
{
	writeFile(path("project.mcvproj"), "  \n {\"formatVersion\": 1}");

	/* The legacy INI format starts with a section header. */
	writeFile(path("legacy.cfg"), "[settings]\nversion=1\n");

	writeFile(path("empty.cfg"), "");

	EXPECT_TRUE(ProjectHandler::isJsonProjectFile(path("project.mcvproj")));
	EXPECT_FALSE(ProjectHandler::isJsonProjectFile(path("legacy.cfg")));
	EXPECT_FALSE(ProjectHandler::isJsonProjectFile(path("empty.cfg")));
	EXPECT_FALSE(ProjectHandler::isJsonProjectFile(path("does-not-exist.mcvproj")));
}

TEST_F(ProjectHandlerTest, savingRequiredFollowsTheEdits)
{
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler handler(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	ProjectData data;
	data.elfPath = "target.elf";

	const std::string file = path("dirty.mcvproj");
	ASSERT_TRUE(handler.save(file, data));

	/* Right after a save the buffer matches the file. */
	EXPECT_FALSE(handler.isSavingRequired(data));

	ProjectData modified = data;
	modified.viewer.sampleFrequencyHz = 999;
	EXPECT_TRUE(handler.isSavingRequired(modified));

	ASSERT_TRUE(handler.save(file, modified));
	EXPECT_FALSE(handler.isSavingRequired(modified));

	/* A freshly started, never saved project counts as modified. */
	handler.reset();
	EXPECT_TRUE(handler.isSavingRequired(modified));
}

TEST_F(ProjectHandlerTest, saveIsAtomicAndLeavesNoTemporaryFileBehind)
{
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotGroupHandler plotGroupHandler;
	ProjectHandler handler(&variableHandler, &plotHandler, &plotGroupHandler, logger.get());

	const std::string file = path("atomic.mcvproj");

	ASSERT_TRUE(handler.save(file, ProjectData{}));
	EXPECT_TRUE(std::filesystem::exists(file));

	/* Overwriting has to succeed as well and must not leave the staging file. */
	ASSERT_TRUE(handler.save(file, ProjectData{}));
	EXPECT_TRUE(std::filesystem::exists(file));
	EXPECT_FALSE(std::filesystem::exists(file + ".tmp"));
}

TEST_F(ProjectHandlerTest, recentProjectsAreMovedToFrontAndCapped)
{
	GlobalConfig globalConfig(logger.get());
	GlobalConfig::Settings& settings = globalConfig.getSettings();

	for (uint32_t index = 0; index < GlobalConfig::maxRecentProjects + 3; index++)
		globalConfig.addRecentProject("project" + std::to_string(index) + ".mcvproj");

	ASSERT_EQ(settings.recentProjects.size(), GlobalConfig::maxRecentProjects);

	/* The newest entry is first, the oldest ones were pushed out. */
	EXPECT_NE(settings.recentProjects.front().find("project12.mcvproj"), std::string::npos);
	EXPECT_NE(settings.recentProjects.back().find("project3.mcvproj"), std::string::npos);
	EXPECT_EQ(settings.recentProjects.front().find("project0.mcvproj"), std::string::npos);

	/* Re-adding an existing entry moves it to the front instead of duplicating it. */
	const std::string repeated = settings.recentProjects.back();
	globalConfig.addRecentProject(repeated);

	EXPECT_EQ(settings.recentProjects.size(), GlobalConfig::maxRecentProjects);
	EXPECT_EQ(settings.recentProjects.front(), repeated);

	const auto occurrences = std::count(settings.recentProjects.begin(), settings.recentProjects.end(), repeated);
	EXPECT_EQ(occurrences, 1);
}

TEST_F(ProjectHandlerTest, missingRecentProjectsAreDropped)
{
	GlobalConfig globalConfig(logger.get());
	GlobalConfig::Settings& settings = globalConfig.getSettings();

	const std::string existing = path("present.mcvproj");
	writeFile(existing, "{}");

	globalConfig.addRecentProject(existing);
	globalConfig.addRecentProject(path("gone.mcvproj"));

	ASSERT_EQ(settings.recentProjects.size(), 2u);

	globalConfig.removeMissingRecentProjects();

	ASSERT_EQ(settings.recentProjects.size(), 1u);
	EXPECT_NE(settings.recentProjects.front().find("present.mcvproj"), std::string::npos);
}
