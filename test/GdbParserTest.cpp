#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "ElfParserFactory.hpp"
#include "GdbParser.hpp"
#include "IElfParser.hpp"
#include "LibDwarfParser.hpp"
#include "TiOfdParser.hpp"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/spdlog.h"

/*
 * The parser interface and the names that stand for the parsers.
 *
 * The names are a contract with three other places: the project file stores
 * one, the settings window shows a label for it, and the API accepts one. A
 * name that the reader of the setting does not recognise leaves the application
 * without a parser, and a label that does not match its parser shows the user
 * the wrong one, so both are pinned down here.
 *
 * The parsing itself needs a file, a compiler toolchain and - for one of the
 * three - a program from TI, so it is covered elsewhere. What is covered here
 * is everything that works without any of them.
 */

namespace
{
	std::shared_ptr<spdlog::logger> makeSilentLogger()
	{
		return std::make_shared<spdlog::logger>("parser-test", std::make_shared<spdlog::sinks::null_sink_mt>());
	}
}  // namespace

TEST(GdbParserTest, theNameIsTheOneTheSettingStores)
{
	const auto logger = makeSilentLogger();

	GdbParser parser(nullptr, logger.get());

	EXPECT_EQ(parser.getName(), "gdb");
	EXPECT_EQ(parser.getType(), IElfParser::Type::Gdb);
	EXPECT_EQ(parser.getDescription(), "GDB: reliable, default.");
}

TEST(GdbParserTest, theCommandIsASetting)
{
	const auto logger = makeSilentLogger();

	GdbParser parser(nullptr, logger.get());

	EXPECT_EQ(parser.getCurrentGDBCommand(), "gdb");

	parser.changeCurrentGDBCommand("C:/tools/arm-none-eabi-gdb.exe");
	EXPECT_EQ(parser.getCurrentGDBCommand(), "C:/tools/arm-none-eabi-gdb.exe");
}

TEST(GdbParserTest, aProgramThatDoesNotAnswerLikeGdbIsRefused)
{
	const auto logger = makeSilentLogger();

	GdbParser parser(nullptr, logger.get());

	/* A name that is not a program at all. The point is that the check comes
	   back with a reason instead of failing later, when the user is waiting for
	   a parse that will not happen. */
	parser.changeCurrentGDBCommand("definitely-not-a-program-anywhere");

	std::string reason;

	EXPECT_FALSE(parser.checkAvailability(reason));
	EXPECT_FALSE(reason.empty());
}

TEST(ElfParserNameTest, everyNameRoundTrips)
{
	for (const IElfParser::Type type : ElfParserFactory::availableTypes())
	{
		IElfParser::Type parsed = IElfParser::Type::TiOfd;

		ASSERT_TRUE(IElfParser::typeFromName(IElfParser::nameOf(type), parsed));
		EXPECT_EQ(parsed, type);
	}
}

TEST(ElfParserNameTest, aNameNoParserAnswersToIsRefused)
{
	IElfParser::Type type = IElfParser::Type::Gdb;

	EXPECT_FALSE(IElfParser::typeFromName("", type));
	EXPECT_FALSE(IElfParser::typeFromName("gcc", type));
	EXPECT_FALSE(IElfParser::typeFromName("GDB", type));
}

TEST(ElfParserNameTest, theDescriptionsAreTheOnesTheWindowShows)
{
	EXPECT_EQ(IElfParser::descriptionOf(IElfParser::Type::Gdb), "GDB: reliable, default.");
	EXPECT_EQ(IElfParser::descriptionOf(IElfParser::Type::LibDwarf), "DWARF: faster variable address updates (beta).");
	EXPECT_EQ(IElfParser::descriptionOf(IElfParser::Type::TiOfd), "C2000: for TI C2000 targets, uses ofd2000.");
}

TEST(ElfParserFactoryTest, eachNameBuildsTheParserItNames)
{
	const auto logger = makeSilentLogger();

	const std::shared_ptr<IElfParser> gdb = ElfParserFactory::create("gdb", nullptr, logger.get());
	const std::shared_ptr<IElfParser> dwarf = ElfParserFactory::create("libdwarf", nullptr, logger.get());
	const std::shared_ptr<IElfParser> c2000 = ElfParserFactory::create("c2000", nullptr, logger.get());

	ASSERT_NE(gdb, nullptr);
	ASSERT_NE(dwarf, nullptr);
	ASSERT_NE(c2000, nullptr);

	EXPECT_EQ(gdb->getType(), IElfParser::Type::Gdb);
	EXPECT_EQ(dwarf->getType(), IElfParser::Type::LibDwarf);
	EXPECT_EQ(c2000->getType(), IElfParser::Type::TiOfd);
}

TEST(ElfParserFactoryTest, aNameThatMeansNothingFallsBackToTheDefault)
{
	/* A project file written by another build, or edited by hand, must not leave
	   the application without a parser. */
	const auto logger = makeSilentLogger();

	const std::shared_ptr<IElfParser> parser = ElfParserFactory::create("something-else", nullptr, logger.get());

	ASSERT_NE(parser, nullptr);
	EXPECT_EQ(parser->getType(), IElfParser::Type::Gdb);
}

TEST(ElfParserFactoryTest, theOrderPutsTheDefaultFirst)
{
	const std::vector<IElfParser::Type> types = ElfParserFactory::availableTypes();

	ASSERT_FALSE(types.empty());
	EXPECT_EQ(types.front(), IElfParser::Type::Gdb);
	EXPECT_EQ(types.size(), 3u);
}

TEST(ElfParserFactoryTest, theLabelsAreShortAndDistinct)
{
	EXPECT_EQ(ElfParserFactory::labelOf(IElfParser::Type::Gdb), "GDB");
	EXPECT_EQ(ElfParserFactory::labelOf(IElfParser::Type::LibDwarf), "DWARF");
	EXPECT_EQ(ElfParserFactory::labelOf(IElfParser::Type::TiOfd), "C2000");
}

TEST(ElfParserFactoryTest, theSettingsReachTheParserThatUsesThem)
{
	const auto logger = makeSilentLogger();

	const std::shared_ptr<IElfParser> gdb = ElfParserFactory::create("gdb", nullptr, logger.get());
	const std::shared_ptr<IElfParser> c2000 = ElfParserFactory::create("c2000", nullptr, logger.get());

	ElfParserFactory::applySettings(*gdb, "arm-none-eabi-gdb", "C:/ti/ofd2000.exe");
	ElfParserFactory::applySettings(*c2000, "arm-none-eabi-gdb", "C:/ti/ofd2000.exe");

	EXPECT_EQ(dynamic_cast<GdbParser*>(gdb.get())->getCurrentGDBCommand(), "arm-none-eabi-gdb");
	EXPECT_EQ(dynamic_cast<TiOfdParser*>(c2000.get())->getToolCommand(), "C:/ti/ofd2000.exe");

	/* The parser that runs no program is handed nothing and changes nothing. */
	const std::shared_ptr<IElfParser> dwarf = ElfParserFactory::create("libdwarf", nullptr, logger.get());
	ElfParserFactory::applySettings(*dwarf, "arm-none-eabi-gdb", "C:/ti/ofd2000.exe");

	EXPECT_EQ(dwarf->getName(), "libdwarf");
}

TEST(ElfParserFactoryTest, whetherTheDwarfReaderIsInThisBuildIsStated)
{
	/* The reader is the one optional dependency. Either answer is correct, but
	   the answer has to be consistent between the two ways of asking, because
	   the settings window asks one and the API asks the other. */
	EXPECT_EQ(LibDwarfParser::isCompiledIn(), LibDwarfParser::isCompiledIn());

	if (!LibDwarfParser::isCompiledIn())
	{
		const auto logger = makeSilentLogger();
		const std::shared_ptr<IElfParser> parser = ElfParserFactory::create("libdwarf", nullptr, logger.get());

		std::string reason;

		EXPECT_FALSE(parser->checkAvailability(reason));
		EXPECT_FALSE(reason.empty());
	}
}
