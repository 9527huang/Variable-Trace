#include "RtosFactory.hpp"

#include "FreeRtosProfile.hpp"

namespace RtosFactory
{
	std::shared_ptr<RtosProfileBase> create(RtosProfileBase::Type type)
	{
		switch (type)
		{
			case RtosProfileBase::Type::FreeRtos:
				return std::make_shared<FreeRtosProfile>();

			case RtosProfileBase::Type::Zephyr:
				/* Named but not built yet. Answering with nothing is better
				   than answering with FreeRTOS, because a caller that gets a
				   profile has no way left to tell it asked for another one. */
				return nullptr;

			case RtosProfileBase::Type::Auto:
			default:
				return nullptr;
		}
	}

	std::shared_ptr<RtosProfileBase> create(const std::string& name)
	{
		RtosProfileBase::Type type = RtosProfileBase::Type::Auto;

		if (!RtosProfileBase::typeFromName(name, type))
			return nullptr;

		return create(type);
	}

	std::vector<RtosProfileBase::Type> availableTypes()
	{
		/* Only what can actually be built. A kernel that appears in the list
		   and then cannot be created is worse than one that is absent. */
		return {RtosProfileBase::Type::FreeRtos};
	}

	std::string labelOf(RtosProfileBase::Type type)
	{
		switch (type)
		{
			case RtosProfileBase::Type::FreeRtos:
				return "FreeRTOS";

			case RtosProfileBase::Type::Zephyr:
				return "Zephyr";

			case RtosProfileBase::Type::Auto:
			default:
				return "Auto";
		}
	}
}  // namespace RtosFactory
