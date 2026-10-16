#include <gtest/gtest.h>

#include <string>

#include "FreeRtosProfile.hpp"
#include "RtosFactory.hpp"

namespace
{
	using Type = RtosProfileBase::Type;
	using Need = RtosProfileBase::Need;
	using Origin = RtosLayoutTable::Origin;

	/* Every symbol the profile asks for, at addresses that step by four, so a
	   mix up between two of them shows up as the wrong address rather than as
	   an accidental match. */
	IElfParser::SymbolMap allSymbols()
	{
		FreeRtosProfile profile;
		IElfParser::SymbolMap symbols;
		uint32_t address = 0x20000000u;

		for (const auto& spec : profile.getSymbols())
		{
			if (spec.name.empty())
				continue;

			symbols[spec.name] = IElfParser::Symbol{address, Variable::Type::U32};
			address += 4u;
		}

		return symbols;
	}

	IElfParser::SymbolMap symbolsWithout(const std::string& name)
	{
		IElfParser::SymbolMap symbols = allSymbols();
		symbols.erase(name);

		return symbols;
	}

	const RtosProfileBase::SymbolSpec* findSymbol(const RtosProfileBase& profile, const std::string& key)
	{
		for (const auto& spec : profile.getSymbols())
		{
			if (spec.key == key)
				return &spec;
		}

		return nullptr;
	}

	const RtosProfileBase::FieldSpec* findField(const RtosProfileBase& profile, const std::string& key)
	{
		for (const auto& spec : profile.getFields())
		{
			if (spec.key == key)
				return &spec;
		}

		return nullptr;
	}
}  // namespace

TEST(FreeRtosProfile, ReportsTheKernelItDescribes)
{
	FreeRtosProfile profile;

	EXPECT_EQ(profile.getType(), Type::FreeRtos);
	EXPECT_EQ(profile.getDescription(), "FreeRTOS");

	/* FreeRTOS numbers priorities upwards. A reader that got this backwards
	   would order every task lane the wrong way round. */
	EXPECT_TRUE(profile.higherNumberIsHigherPriority());
	EXPECT_EQ(profile.taskNameCapacity(), 16u);
}

TEST(FreeRtosProfile, NamesOnlySymbolsTheKernelActuallyHas)
{
	FreeRtosProfile profile;

	ASSERT_FALSE(profile.getSymbols().empty());

	for (const auto& spec : profile.getSymbols())
	{
		EXPECT_FALSE(spec.key.empty());
		EXPECT_FALSE(spec.name.empty()) << spec.key << " has no symbol name";
		EXPECT_FALSE(spec.purpose.empty()) << spec.key << " has no stated purpose";

		/* The kernel version is a macro, not a symbol: it only reaches the
		   object file as a string literal, and only reaches the debug sections
		   when the build carried macro debug information. Asking for it as a
		   symbol would fail on every target that did not. */
		EXPECT_NE(spec.name, "tskKERNEL_VERSION_NUMBER");
	}
}

TEST(FreeRtosProfile, MarksWhatCanBeAbsent)
{
	FreeRtosProfile profile;

	const auto* currentTask = findSymbol(profile, "currentTask");
	ASSERT_NE(currentTask, nullptr);
	EXPECT_EQ(currentTask->need, Need::Required);

	const auto* tickCount = findSymbol(profile, "tickCount");
	ASSERT_NE(tickCount, nullptr);
	EXPECT_EQ(tickCount->need, Need::Required);

	/* Present only when optimised task selection is on. */
	const auto* topReady = findSymbol(profile, "topReadyPriority");
	ASSERT_NE(topReady, nullptr);
	EXPECT_EQ(topReady->need, Need::Optional);

	/* Present only when trace facility is on. */
	const auto* waiting = findSymbol(profile, "waitingTermination");
	ASSERT_NE(waiting, nullptr);
	EXPECT_EQ(waiting->need, Need::Optional);
}

/*
 * The fallback table plus the one field it deliberately leaves out of itself.
 *
 * The run time counter has no written down offset on purpose: where it sits
 * depends on which optional fields the build left out, and a counter read from
 * a neighbouring word would be a share of nothing. A test that means "the
 * layout describes everything" has to add it, the way a target built with run
 * time statistics describes it.
 */
RtosLayoutTable layoutWithRunTimeCounter(const FreeRtosProfile& profile)
{
	RtosLayoutTable layout = profile.getFallbackLayout();

	layout.addStruct("TCB_t", 100, Origin::Measured);
	layout.addField("TCB_t", "ulRunTimeCounter", 88, Origin::Measured);

	return layout;
}

TEST(FreeRtosProfile, ResolvesEverythingWhenTheSymbolFileHasItAll)
{
	FreeRtosProfile profile;
	RtosLayoutTable layout = layoutWithRunTimeCounter(profile);

	RtosProfileBase::Resolution result = profile.resolve(allSymbols(), layout);

	EXPECT_TRUE(result.complete());
	EXPECT_TRUE(result.missingRequired.empty());
	EXPECT_TRUE(result.missingOptional.empty());

	EXPECT_TRUE(result.has("currentTask"));
	EXPECT_TRUE(result.has("tickCount"));

	/* The addresses come from the symbol map, not from the layout. */
	EXPECT_EQ(result.addressOf("currentTask"), 0x20000000u);
	EXPECT_EQ(result.addressOf("tickCount"), 0x20000028u);
}

TEST(FreeRtosProfile, StopsAndNamesWhatIsMissing)
{
	FreeRtosProfile profile;
	RtosLayoutTable layout = profile.getFallbackLayout();

	RtosProfileBase::Resolution result = profile.resolve(symbolsWithout("pxReadyTasksLists"), layout);

	EXPECT_FALSE(result.complete());
	ASSERT_EQ(result.missingRequired.size(), 1u);

	/* The name and what it was wanted for, so that the interface can say what
	   to do rather than only that something is missing. */
	EXPECT_NE(result.missingRequired.front().find("pxReadyTasksLists"), std::string::npos);
	EXPECT_FALSE(result.has("readyLists"));
}

TEST(FreeRtosProfile, KeepsGoingWhenAnOptionalSymbolIsMissing)
{
	FreeRtosProfile profile;
	RtosLayoutTable layout = layoutWithRunTimeCounter(profile);

	RtosProfileBase::Resolution result = profile.resolve(symbolsWithout("uxTopReadyPriority"), layout);

	EXPECT_TRUE(result.complete());
	ASSERT_EQ(result.missingOptional.size(), 1u);
	EXPECT_NE(result.missingOptional.front().find("uxTopReadyPriority"), std::string::npos);
	EXPECT_FALSE(result.has("topReadyPriority"));
}

TEST(FreeRtosProfile, FindsTheContainerPointerUnderEitherName)
{
	FreeRtosProfile profile;

	/* The field was renamed between V10 and V11 without moving. A symbol file
	   from the older release answers to the older spelling only. */
	RtosLayoutTable layout;
	layout.addField("ListItem_t", "pvContainer", 16, Origin::Measured);

	RtosProfileBase::Resolution result = profile.resolve(allSymbols(), layout);

	const auto it = result.fields.find("listItem.container");
	ASSERT_NE(it, result.fields.end());
	EXPECT_TRUE(it->second.resolved);
	EXPECT_EQ(it->second.resolvedName, "pvContainer");
	EXPECT_EQ(it->second.offset, 16u);

	/* It answered from debug information, so it is not a fallback. */
	EXPECT_FALSE(it->second.usedFallback);
	EXPECT_FALSE(result.anyFallbackOffset());
}

TEST(FreeRtosProfile, PrefersTheNewerNameWhenBothArePresent)
{
	FreeRtosProfile profile;

	RtosLayoutTable layout;
	layout.addField("ListItem_t", "pxContainer", 16, Origin::Measured);
	layout.addField("ListItem_t", "pvContainer", 20, Origin::Measured);

	RtosProfileBase::Resolution result = profile.resolve(allSymbols(), layout);

	const auto it = result.fields.find("listItem.container");
	ASSERT_NE(it, result.fields.end());
	EXPECT_TRUE(it->second.resolved);
	EXPECT_EQ(it->second.resolvedName, "pxContainer");
	EXPECT_EQ(it->second.offset, 16u);
}

TEST(FreeRtosProfile, MarksOffsetsThatCameFromTheBuiltInTable)
{
	FreeRtosProfile profile;
	RtosLayoutTable layout = profile.getFallbackLayout();

	RtosProfileBase::Resolution result = profile.resolve(allSymbols(), layout);

	EXPECT_TRUE(result.anyFallbackOffset());
	EXPECT_FALSE(result.fallbackFields.empty());

	/* Every field the table answered for is marked, because every number in it
	   was written down rather than read. The run time counter is the one field
	   it does not answer for, so this is one short of the whole table. */
	EXPECT_EQ(result.fallbackFields.size(), profile.getFields().size() - 1);
	EXPECT_TRUE(layout.isAllFallback());

	/* And it is left unresolved rather than resolved to a guess, which is the
	   point of leaving it out: no column is better than a column of words read
	   from beside the field that should have been there. */
	uint32_t offset = 0;
	EXPECT_FALSE(result.offsetOf("tcb.runTimeCounter", offset));
}

TEST(FreeRtosProfile, DoesNotMarkOffsetsThatWereMeasured)
{
	FreeRtosProfile profile;

	RtosLayoutTable layout;

	for (const auto& spec : profile.getFields())
		layout.addField(spec.structName, spec.altNames.front(), 0, Origin::Measured);

	RtosProfileBase::Resolution result = profile.resolve(allSymbols(), layout);

	EXPECT_TRUE(result.complete());
	EXPECT_FALSE(result.anyFallbackOffset());
	EXPECT_TRUE(result.fallbackFields.empty());
}

TEST(FreeRtosProfile, FallbackLayoutMatchesTheMeasuredControlBlock)
{
	FreeRtosProfile profile;
	RtosLayoutTable layout = profile.getFallbackLayout();

	uint32_t size = 0;
	ASSERT_TRUE(layout.structSize("TCB_t", size));
	EXPECT_EQ(size, 96u);

	ASSERT_TRUE(layout.structSize("ListItem_t", size));
	EXPECT_EQ(size, 20u);

	ASSERT_TRUE(layout.structSize("List_t", size));
	EXPECT_EQ(size, 20u);

	RtosProfileBase::Resolution result = profile.resolve(allSymbols(), layout);

	/* The offsets the plan fixed in 22.4, which the control block layout
	   measurements produced. */
	uint32_t offset = 0;

	ASSERT_TRUE(result.offsetOf("listItem.value", offset));
	EXPECT_EQ(offset, 0u);

	ASSERT_TRUE(result.offsetOf("listItem.owner", offset));
	EXPECT_EQ(offset, 12u);

	ASSERT_TRUE(result.offsetOf("listItem.container", offset));
	EXPECT_EQ(offset, 16u);

	ASSERT_TRUE(result.offsetOf("list.end", offset));
	EXPECT_EQ(offset, 8u);

	ASSERT_TRUE(result.offsetOf("miniItem.next", offset));
	EXPECT_EQ(offset, 4u);

	ASSERT_TRUE(result.offsetOf("tcb.stateListItem", offset));
	EXPECT_EQ(offset, 4u);

	ASSERT_TRUE(result.offsetOf("tcb.eventListItem", offset));
	EXPECT_EQ(offset, 24u);

	ASSERT_TRUE(result.offsetOf("tcb.priority", offset));
	EXPECT_EQ(offset, 44u);

	ASSERT_TRUE(result.offsetOf("tcb.stack", offset));
	EXPECT_EQ(offset, 48u);

	ASSERT_TRUE(result.offsetOf("tcb.taskName", offset));
	EXPECT_EQ(offset, 52u);

	ASSERT_TRUE(result.offsetOf("tcb.endOfStack", offset));
	EXPECT_EQ(offset, 68u);

	ASSERT_TRUE(result.offsetOf("tcb.basePriority", offset));
	EXPECT_EQ(offset, 80u);
}

TEST(FreeRtosProfile, FieldSpecsNameAStructureAndAtLeastOneField)
{
	FreeRtosProfile profile;

	ASSERT_FALSE(profile.getFields().empty());

	for (const auto& spec : profile.getFields())
	{
		EXPECT_FALSE(spec.key.empty());
		EXPECT_FALSE(spec.structName.empty()) << spec.key;
		EXPECT_FALSE(spec.altNames.empty()) << spec.key << " has no field name to try";

		/* A missing field is reported by the name that was tried, which is the
		   first one. An empty entry there would produce a message about a
		   field with no name. */
		EXPECT_FALSE(spec.altNames.front().empty()) << spec.key;
	}
}

TEST(RtosFactory, BuildsOnlyWhatExists)
{
	EXPECT_NE(RtosFactory::create(Type::FreeRtos), nullptr);

	/* Named but not written yet. Answering with nothing is better than
	   answering with another kernel's table. */
	EXPECT_EQ(RtosFactory::create(Type::Zephyr), nullptr);
	EXPECT_EQ(RtosFactory::create(Type::Auto), nullptr);

	EXPECT_NE(RtosFactory::create("freertos"), nullptr);
	EXPECT_NE(RtosFactory::create("FreeRTOS"), nullptr);
	EXPECT_NE(RtosFactory::create("FREERTOS"), nullptr);

	EXPECT_EQ(RtosFactory::create("auto"), nullptr);
	EXPECT_EQ(RtosFactory::create("zephyr"), nullptr);
	EXPECT_EQ(RtosFactory::create("nosuchkernel"), nullptr);
	EXPECT_EQ(RtosFactory::create(""), nullptr);
}

TEST(RtosFactory, OffersOnlyWhatItCanBuild)
{
	const std::vector<Type> types = RtosFactory::availableTypes();

	ASSERT_EQ(types.size(), 1u);
	EXPECT_EQ(types.front(), Type::FreeRtos);

	for (Type type : types)
		EXPECT_NE(RtosFactory::create(type), nullptr);

	EXPECT_EQ(RtosFactory::labelOf(Type::FreeRtos), "FreeRTOS");
}
