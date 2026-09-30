#ifndef _WRITEPLANNER_HPP
#define _WRITEPLANNER_HPP

#include <cstddef>
#include <string>
#include <vector>

/*
 * A list of named write plans.
 *
 * A plan is a sequence of (time, value) steps for one variable, plus the curve
 * those steps describe. The preview in the window and the numbers in the table
 * come from the same list, so the two can never disagree.
 *
 * The times are the plan's own axis and start at zero: a step at 1.5 s is a
 * second and a half into the plan, not a moment on the acquisition clock.
 */
class WritePlanner
{
   public:
	struct Step
	{
		double time = 0.0;
		double value = 0.0;

		bool operator==(const Step& other) const
		{
			return time == other.time && value == other.value;
		}
	};

	struct Plan
	{
		std::string name = "";
		std::string variable = "";
		std::vector<Step> steps{};
	};

	/* Bounds that keep a plan editable by hand and small enough to save with the
	   project without turning it into a data file. */
	static constexpr size_t maximumPlans = 64;
	static constexpr size_t maximumStepsPerPlan = 512;

	/* The first name of the form base0, base1, ... that is still free. */
	static std::string freeName(const WritePlanner& planner, const std::string& base);

	bool addPlan(const std::string& name, const std::string& variable = "");
	bool removePlan(const std::string& name);
	bool renamePlan(const std::string& oldName, const std::string& newName);
	bool setVariable(const std::string& name, const std::string& variable);

	/* Replaces the whole list, used when a project is loaded and by the table
	   editor. Steps are kept in the order they arrive: the user edits rows in
	   place, and reordering them under the cursor would be worse than useless.
	   The order does not affect valueAt(), which looks the time up rather than
	   walking the list. */
	bool setSteps(const std::string& name, const std::vector<Step>& steps);
	bool addStep(const std::string& name, double time, double value);
	bool removeStep(const std::string& name, size_t index);

	bool hasPlan(const std::string& name) const;
	Plan getPlan(const std::string& name) const;
	std::vector<std::string> getNames() const;
	size_t size() const;
	void clear();

	/* The value the plan holds at a moment: the value of the step with the
	   greatest time that is not later than the one asked for, and the value of
	   the first step before the plan begins. Zero when there are no steps. */
	static double valueAt(const Plan& plan, double time);

	/* The time of the last step, which is how long the plan lasts. Zero for a
	   plan without steps. */
	static double duration(const Plan& plan);

   private:
	std::vector<Plan> plans;
	std::vector<Plan>::iterator find(const std::string& name);
	std::vector<Plan>::const_iterator find(const std::string& name) const;
};

#endif
