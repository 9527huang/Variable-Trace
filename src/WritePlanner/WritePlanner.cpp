#include "WritePlanner.hpp"

#include <algorithm>

std::string WritePlanner::freeName(const WritePlanner& planner, const std::string& base)
{
	for (size_t index = 0;; index++)
	{
		const std::string candidate = base + std::to_string(index);

		if (!planner.hasPlan(candidate))
			return candidate;
	}
}

std::vector<WritePlanner::Plan>::iterator WritePlanner::find(const std::string& name)
{
	return std::find_if(plans.begin(), plans.end(), [&name](const Plan& plan)
						{ return plan.name == name; });
}

std::vector<WritePlanner::Plan>::const_iterator WritePlanner::find(const std::string& name) const
{
	return std::find_if(plans.begin(), plans.end(), [&name](const Plan& plan)
						{ return plan.name == name; });
}

bool WritePlanner::addPlan(const std::string& name, const std::string& variable)
{
	if (name.empty() || hasPlan(name) || plans.size() >= maximumPlans)
		return false;

	plans.push_back(Plan{name, variable, {}});
	return true;
}

bool WritePlanner::removePlan(const std::string& name)
{
	auto entry = find(name);

	if (entry == plans.end())
		return false;

	plans.erase(entry);
	return true;
}

bool WritePlanner::renamePlan(const std::string& oldName, const std::string& newName)
{
	if (newName.empty() || oldName == newName || hasPlan(newName))
		return false;

	auto entry = find(oldName);

	if (entry == plans.end())
		return false;

	entry->name = newName;
	return true;
}

bool WritePlanner::setVariable(const std::string& name, const std::string& variable)
{
	auto entry = find(name);

	if (entry == plans.end())
		return false;

	entry->variable = variable;
	return true;
}

bool WritePlanner::setSteps(const std::string& name, const std::vector<Step>& steps)
{
	auto entry = find(name);

	if (entry == plans.end())
		return false;

	entry->steps = steps;

	/* The bound is applied here rather than refused, because a hand written
	   project file should still open with the first steps intact. */
	if (entry->steps.size() > maximumStepsPerPlan)
		entry->steps.resize(maximumStepsPerPlan);

	return true;
}

bool WritePlanner::addStep(const std::string& name, double time, double value)
{
	auto entry = find(name);

	if (entry == plans.end() || entry->steps.size() >= maximumStepsPerPlan)
		return false;

	entry->steps.push_back(Step{time, value});
	return true;
}

bool WritePlanner::removeStep(const std::string& name, size_t index)
{
	auto entry = find(name);

	if (entry == plans.end() || index >= entry->steps.size())
		return false;

	entry->steps.erase(entry->steps.begin() + static_cast<std::ptrdiff_t>(index));
	return true;
}

bool WritePlanner::hasPlan(const std::string& name) const
{
	return find(name) != plans.end();
}

WritePlanner::Plan WritePlanner::getPlan(const std::string& name) const
{
	auto entry = find(name);

	if (entry == plans.end())
		return Plan{};

	return *entry;
}

std::vector<std::string> WritePlanner::getNames() const
{
	std::vector<std::string> names;
	names.reserve(plans.size());

	for (const Plan& plan : plans)
		names.push_back(plan.name);

	return names;
}

size_t WritePlanner::size() const
{
	return plans.size();
}

void WritePlanner::clear()
{
	plans.clear();
}

double WritePlanner::valueAt(const Plan& plan, double time)
{
	if (plan.steps.empty())
		return 0.0;

	const Step* best = nullptr;

	for (const Step& step : plan.steps)
	{
		if (step.time > time)
			continue;

		/* The newest step that has already happened wins, and for two steps at
		   the same moment the one later in the list wins, which is the one the
		   user typed last. */
		if (best == nullptr || step.time >= best->time)
			best = &step;
	}

	if (best == nullptr)
		best = &plan.steps.front();

	return best->value;
}

double WritePlanner::duration(const Plan& plan)
{
	double longest = 0.0;

	for (const Step& step : plan.steps)
		longest = std::max(longest, step.time);

	return longest;
}
