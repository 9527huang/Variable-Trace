#ifndef _RTOSFACTORY_HPP
#define _RTOSFACTORY_HPP

#include <memory>
#include <string>
#include <vector>

#include "RtosProfileBase.hpp"

/*
 * The one place that decides which profile a kernel name means.
 *
 * The setting stores a name, the profile stores an enumerator, and the
 * interface, the project file and the API each hold one of the two. Turning
 * any of them into an object in one place is what keeps a name the setting
 * accepts from being a name that cannot be built.
 */
namespace RtosFactory
{
	std::shared_ptr<RtosProfileBase> create(RtosProfileBase::Type type);

	/* Answers with nothing for a name that means "let detection decide", and
	   for a name no profile answers to. A caller that needs a profile has to
	   treat the empty result as a reason to run detection rather than as a
	   failure, which is why the two cases are told apart by the log line and
	   not by the return value. */
	std::shared_ptr<RtosProfileBase> create(const std::string& name);

	/* The concrete kinds, in the order the settings window offers them. `Auto`
	   is not among them: it names a request, not a kernel. */
	std::vector<RtosProfileBase::Type> availableTypes();

	/* The label the settings window shows. */
	std::string labelOf(RtosProfileBase::Type type);
}  // namespace RtosFactory

#endif
