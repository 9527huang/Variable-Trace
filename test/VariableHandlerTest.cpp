#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "Variable.hpp"
#include "VariableHandler.hpp"

/*
 * A variable is named by the plots that draw it through bare pointers, so
 * deleting one is two changes: the series that point at it come off the plots,
 * and then the object goes. Both halves have to be one step as far as the
 * acquisition thread is concerned, which reads the same model on every sample -
 * it must not find a series whose variable has already been freed, and it must
 * not walk the collection while it is being changed.
 *
 * The tests below pin the order of those two halves down, and check that the
 * lock that makes the step indivisible is really taken by the collection itself
 * rather than left to the caller to remember.
 */

TEST(VariableHandlerTest, deletingAVariableDropsTheSeriesThatNamedItFirst)
{
	VariableHandler handler;
	handler.addNewVariable("speed");

	std::string notified;
	bool variableWasStillThere = false;

	handler.removeSeriesCallback = [&](const std::string& name)
	{
		notified = name;
		/* The whole point of the order: a series holds a bare pointer, so it can
		   only be sent to the plots while the object it points at is still in
		   the collection. */
		variableWasStillThere = handler.contains(name);
	};

	handler.erase("speed");

	EXPECT_EQ(notified, "speed");
	EXPECT_TRUE(variableWasStillThere);
	EXPECT_FALSE(handler.contains("speed"));
}

TEST(VariableHandlerTest, deletingAVariableAsksAboutNothingElse)
{
	VariableHandler handler;
	handler.addNewVariable("speed");
	handler.addNewVariable("current");

	int asked = 0;
	handler.removeSeriesCallback = [&](const std::string&) { asked++; };

	handler.erase("speed");

	EXPECT_EQ(asked, 1);
	EXPECT_TRUE(handler.contains("current"));
}

TEST(VariableHandlerTest, deletingAVariableWithoutTheCallbackStillWorks)
{
	VariableHandler handler;
	handler.addNewVariable("speed");

	handler.erase("speed");

	EXPECT_FALSE(handler.contains("speed"));
}

TEST(VariableHandlerTest, deletingAVariableWaitsForTheWalkInProgress)
{
	VariableHandler handler;
	handler.addNewVariable("speed");

	std::atomic<bool> deleted{false};
	std::thread deleter;

	{
		/* Stands in for the acquisition thread, which holds this while it reads
		   the model on every sample. */
		auto walk = handler.lockModel();

		deleter = std::thread(
			[&]()
			{
				handler.erase("speed");
				deleted = true;
			});

		/* Long enough that a delete which did not wait would have gone through.
		   The check only ever fails in that direction, so a slow machine cannot
		   make it pass wrongly. */
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		EXPECT_FALSE(deleted.load());
		EXPECT_TRUE(handler.contains("speed"));
	}

	deleter.join();

	EXPECT_TRUE(deleted.load());
	EXPECT_FALSE(handler.contains("speed"));
}

TEST(VariableHandlerTest, theRenameCallbackSeesTheNewNameInTheCollection)
{
	VariableHandler handler;
	handler.addNewVariable("speed");

	std::string notified;
	bool collectionWasAlreadyRenamed = false;

	handler.renameCallback = [&](const std::string& oldName, const std::string& newName)
	{
		notified = newName;
		collectionWasAlreadyRenamed = handler.contains(newName) && !handler.contains(oldName);
	};

	handler.renameVariable("speed", "velocity");

	EXPECT_EQ(notified, "velocity");
	EXPECT_TRUE(collectionWasAlreadyRenamed);
	EXPECT_TRUE(handler.contains("velocity"));
}

TEST(VariableHandlerTest, deletingAVariableRepairsWhatNamedIt)
{
	VariableHandler handler;

	handler.addNewVariable("base");
	handler.addNewVariable("scaled");

	/* A fractional variable can be scaled by another variable, which it names
	   with a bare pointer. That pointer has to go when that variable does, or
	   the next transform would read freed memory. */
	const std::shared_ptr<Variable> scaled = handler.getVariable("scaled");
	scaled->setHighLevelType(Variable::HighLevelType::SIGNEDFRAC);

	Variable::Fractional fractional = scaled->getFractional();
	fractional.base = 2.0;
	fractional.baseVariable = handler.getVariable("base").get();
	scaled->setFractional(fractional);

	handler.erase("base");

	EXPECT_EQ(scaled->getFractional().baseVariable, nullptr);
	EXPECT_EQ(scaled->getFractional().base, 1.0);
}
