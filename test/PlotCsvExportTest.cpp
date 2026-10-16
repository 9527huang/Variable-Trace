#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "IFileHandler.hpp"
#include "Plot.hpp"
#include "PlotCsvExport.hpp"
#include "Variable.hpp"
#include "spdlog/sinks/null_sink.h"

/*
 * Two panels offer this export now - the plots tree on the variable side and
 * the channel panel on the trace side - so what it writes is worth pinning
 * down. A column of timestamps comes first and then one column per series, in
 * the order the plot holds them, which is what a reader of the file relies on
 * and what a change made for one panel would otherwise quietly alter for the
 * other.
 */

class AcceptingFileHandler : public IFileHandler
{
   public:
	explicit AcceptingFileHandler(std::string path) : path(std::move(path)) {}

	bool init() override
	{
		return true;
	}

	bool deinit() override
	{
		return true;
	}

	std::string openFile(std::vector<Filter>&&) override
	{
		return {};
	}

	std::string openDirectory(std::vector<Filter>&&) override
	{
		return {};
	}

	std::string saveFile(std::vector<Filter>&&, const std::string& = "", const std::string& = "") override
	{
		return path;
	}

   private:
	std::string path;
};

class PlotCsvExportTest : public ::testing::Test
{
   protected:
	void SetUp() override
	{
		path = std::filesystem::temp_directory_path() / "vt_plot_export_test.csv";
		std::filesystem::remove(path);

		logger = spdlog::get("PlotCsvExportTest");

		/* The registry keeps one logger per name, so the second test would be
		   refused a logger of its own. */
		if (!logger)
			logger = spdlog::null_logger_mt("PlotCsvExportTest");
	}

	void TearDown() override
	{
		std::filesystem::remove(path);
	}

	std::vector<std::string> linesOf()
	{
		std::ifstream exported(path);
		std::vector<std::string> lines{};
		std::string line;

		while (std::getline(exported, line))
			lines.push_back(line);

		return lines;
	}

	std::filesystem::path path{};
	std::shared_ptr<spdlog::logger> logger{};
};

TEST_F(PlotCsvExportTest, theTableCarriesATimeColumnAndOneColumnPerSeries)
{
	auto plot = std::make_shared<Plot>("p");
	Variable first("first");
	Variable second("second");

	plot->addSeries(&first);
	plot->addSeries(&second);

	plot->addPoint("first", 1.5);
	plot->addPoint("second", 2.5);
	plot->addTimePoint(0.1);

	plot->addPoint("first", 3.5);
	plot->addPoint("second", 4.5);
	plot->addTimePoint(0.2);

	AcceptingFileHandler fileHandler(path.string());
	exportPlotToCSV(plot, &fileHandler, logger.get());

	const std::vector<std::string> lines = linesOf();

	ASSERT_EQ(lines.size(), 3);

	EXPECT_EQ(lines[0], "time [s],first,second,");
	EXPECT_EQ(lines[1], "0.1,1.5,2.5,");
	EXPECT_EQ(lines[2], "0.2,3.5,4.5,");
}

TEST_F(PlotCsvExportTest, aDismissedDialogWritesNothing)
{
	auto plot = std::make_shared<Plot>("p");
	Variable only("only");

	plot->addSeries(&only);
	plot->addPoint("only", 1.0);
	plot->addTimePoint(0.1);

	/* An empty path is how the dialog reports that nothing was chosen. */
	AcceptingFileHandler fileHandler("");
	exportPlotToCSV(plot, &fileHandler, logger.get());

	EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(PlotCsvExportTest, aPlotWithNoSamplesStillWritesItsHeader)
{
	auto plot = std::make_shared<Plot>("p");
	Variable idle("idle");

	plot->addSeries(&idle);

	AcceptingFileHandler fileHandler(path.string());
	exportPlotToCSV(plot, &fileHandler, logger.get());

	const std::vector<std::string> lines = linesOf();

	ASSERT_EQ(lines.size(), 1);
	EXPECT_EQ(lines[0], "time [s],idle,");
}

/*
 * The second half of the second milestone's criteria: a queue depth curve can
 * be exported, and what comes out is the depth.
 *
 * The drawing half was checked on a real target. This half is about the file.
 * A derived series has no address on the target, and an export that built its
 * columns out of addresses rather than out of series would leave it out
 * without saying so - the table would look complete and be missing the one
 * column the milestone was about.
 */
TEST_F(PlotCsvExportTest, aDerivedSeriesIsExportedLikeAnyOther)
{
	auto plot = std::make_shared<Plot>("p");
	Variable fromTarget("g_led_ticks");
	Variable workedOutHere("q_sensor[depth]");

	workedOutHere.setDerived(true);

	plot->addSeries(&fromTarget);
	plot->addSeries(&workedOutHere);

	plot->addPoint("g_led_ticks", 100.0);
	plot->addPoint("q_sensor[depth]", 0.0);
	plot->addTimePoint(0.1);

	plot->addPoint("g_led_ticks", 101.0);
	plot->addPoint("q_sensor[depth]", 1.0);
	plot->addTimePoint(0.2);

	AcceptingFileHandler fileHandler(path.string());
	exportPlotToCSV(plot, &fileHandler, logger.get());

	const std::vector<std::string> lines = linesOf();

	ASSERT_EQ(lines.size(), 3);

	EXPECT_EQ(lines[0], "time [s],g_led_ticks,q_sensor[depth],");
	EXPECT_EQ(lines[1], "0.1,100,0,");
	EXPECT_EQ(lines[2], "0.2,101,1,");
}
