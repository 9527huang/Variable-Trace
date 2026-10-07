#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "Plot.hpp"
#include "PlotHandler.hpp"

class PlotGroup
{
   public:
	/* A group of plots the viewer samples over the debug probe, or one whose
	   variables the target recorder captures into its own buffer. The recorder
	   works on whole groups because it has to pack a fixed set of variables into
	   one sample, and a group is that set. */
	enum class Type
	{
		Sampling = 0,
		Recorder = 1,
	};

	struct PlotEntry
	{
		bool visibility = true;
		std::shared_ptr<Plot> plot;
	};

	PlotGroup(const std::string& name, Type type = Type::Sampling) : name(name), type(type)
	{
	}

	Type getType() const
	{
		return type;
	}

	void setType(Type newType)
	{
		type = newType;
	}

	std::string getTypeName() const
	{
		return type == Type::Recorder ? "recorder" : "sampling";
	}

	/* Name of the group this one is nested in, or an empty string when it sits
	   at the top level. Held as a name rather than a pointer so that the tree
	   has one owner - the handler - and no group can outlive a parent through a
	   stale pointer. */
	std::string getParentName() const
	{
		return parentName;
	}

	void setParentName(const std::string& newParentName)
	{
		parentName = newParentName;
	}

	bool isRoot() const
	{
		return parentName.empty();
	}

	void addPlot(std::shared_ptr<Plot> plot, bool visibility = true)
	{
		group[plot->getName()] = {visibility, plot};
	}

	void removePlot(const std::string& name)
	{
		group.erase(name);
	}

	void setVisibility(const std::string& name, bool visible)
	{
		group.at(name).visibility = visible;
	}

	bool getVisibility(const std::string& name) const
	{
		return group.at(name).visibility;
	}

	void setName(const std::string& name)
	{
		this->name = name;
	}

	std::string getName() const
	{
		return name;
	}

	bool renamePlot(const std::string& oldName, const std::string& newName)
	{
		if (!group.contains(oldName))
			return false;

		auto groupElement = group.extract(oldName);
		groupElement.key() = newName;
		group.insert(std::move(groupElement));
		return true;
	}

	std::map<std::string, PlotEntry>::const_iterator begin() const
	{
		return group.cbegin();
	}

	std::map<std::string, PlotEntry>::const_iterator end() const
	{
		return group.cend();
	}

	uint32_t getVisiblePlotsCount() const
	{
		return std::count_if(group.begin(), group.end(), [](const auto& pair)
							 { return pair.second.visibility; });
	}

   private:
	std::string name;
	Type type = Type::Sampling;
	std::string parentName = "";
	std::map<std::string, PlotEntry> group;
};

class PlotGroupHandler
{
   public:
	/* An empty parentName puts the group at the top level. */
	std::shared_ptr<PlotGroup> addGroup(const std::string& name, PlotGroup::Type type = PlotGroup::Type::Sampling, const std::string& parentName = "")
	{
		groupMap[name] = std::make_shared<PlotGroup>(name, type);
		groupMap[name]->setParentName(parentName);
		return groupMap[name];
	}

	void renameGroup(const std::string& oldName, const std::string& newName)
	{
		if (!groupMap.contains(oldName))
			return;

		auto group = groupMap.extract(oldName);
		group.key() = newName;
		groupMap.insert(std::move(group));
		groupMap[newName]->setName(newName);

		/* A child names its parent, so every child of the renamed group has to
		   follow or it would point at a name that no longer exists and drop out
		   of the tree. */
		for (auto& [name, child] : groupMap)
		{
			if (child->getParentName() == oldName)
				child->setParentName(newName);
		}
	}

	/* The names of the groups nested directly in the given one. An empty name
	   lists the top level. */
	std::vector<std::string> getChildNames(const std::string& parentName) const
	{
		std::vector<std::string> names;

		for (const auto& [name, group] : groupMap)
		{
			if (group->getParentName() == parentName)
				names.push_back(name);
		}

		return names;
	}

	std::vector<std::string> getRootNames() const
	{
		return getChildNames("");
	}

	bool hasChildren(const std::string& name) const
	{
		for (const auto& [childName, group] : groupMap)
		{
			if (group->getParentName() == name)
				return true;
		}

		return false;
	}

	/* True when name is the given group itself or sits somewhere below it. The
	   walk stops after as many steps as there are groups, so a project file that
	   describes a cycle cannot make it spin. */
	bool isDescendantOf(const std::string& name, const std::string& ancestorName) const
	{
		std::string current = name;

		for (size_t step = 0; step <= groupMap.size(); step++)
		{
			if (current == ancestorName)
				return true;

			auto entry = groupMap.find(current);

			if (entry == groupMap.end() || entry->second->getParentName().empty())
				return false;

			current = entry->second->getParentName();
		}

		return false;
	}

	/* How far the group sits below the top level, for a caller that wants to
	   indent a name without walking the tree itself. */
	uint32_t getDepth(const std::string& name) const
	{
		uint32_t depth = 0;
		std::string current = name;

		for (size_t step = 0; step <= groupMap.size(); step++)
		{
			auto entry = groupMap.find(current);

			if (entry == groupMap.end() || entry->second->getParentName().empty())
				return depth;

			current = entry->second->getParentName();
			depth++;
		}

		return depth;
	}

	/* Nests a group in another one. Refused when the move would put a group
	   inside itself or inside one of its own children, which would cut the whole
	   branch off the top level. */
	bool moveGroup(const std::string& name, const std::string& newParentName)
	{
		if (!groupMap.contains(name))
			return false;

		if (!newParentName.empty() && !groupMap.contains(newParentName))
			return false;

		if (isDescendantOf(newParentName, name))
			return false;

		groupMap.at(name)->setParentName(newParentName);
		return true;
	}

	void removeGroup(const std::string& name)
	{
		/* A child whose parent is gone would never be drawn again, so the whole
		   branch goes with the group. */
		std::vector<std::string> doomed;

		for (const auto& [groupName, group] : groupMap)
		{
			if (isDescendantOf(groupName, name))
				doomed.push_back(groupName);
		}

		for (const std::string& groupName : doomed)
			groupMap.erase(groupName);

		if (groupMap.size() == 0)
			addGroup("new group0");

		/* The active group only moves when the one it named is gone. Removing
		   some other group should not take the view away from where the user
		   was. */
		if (!groupMap.contains(activeGroup))
			activeGroup = groupMap.begin()->first;
	}

	void removeAllGroups()
	{
		groupMap.clear();
	}

	bool renamePlotInAllGroups(const std::string& oldName, const std::string& newName)
	{
		for (auto& [name, group] : groupMap)
		{
			group->renamePlot(oldName, newName);
		}
		return true;
	}

	size_t getGroupCount()
	{
		return groupMap.size();
	}

	std::shared_ptr<PlotGroup> getGroup(const std::string& name)
	{
		return groupMap.at(name);
	}

	std::map<std::string, std::shared_ptr<PlotGroup>>::const_iterator begin() const
	{
		return groupMap.cbegin();
	}

	std::map<std::string, std::shared_ptr<PlotGroup>>::const_iterator end() const
	{
		return groupMap.cend();
	}

	void setActiveGroup(const std::string& name)
	{
		activeGroup = name;
	}

	/* Name of the active group as it is stored in the project file. Does not
	   repair a stale name, that is left to getActiveGroup(). */
	std::string getActiveGroupName() const
	{
		return activeGroup;
	}

	std::shared_ptr<PlotGroup> getActiveGroup()
	{
		/* An empty handler would make the lookup below dereference end(), so a
		   default group is created on demand. */
		if (groupMap.empty())
		{
			activeGroup = "default group";
			return addGroup(activeGroup);
		}

		if (!groupMap.contains(activeGroup))
			activeGroup = groupMap.begin()->first;

		return groupMap.at(activeGroup);
	}

	/* The group the recorder works on, or nothing when the active group is not
	   one. A recorder packs a fixed set of variables into every sample, so it
	   needs the group the user pointed at rather than any group at all. */
	std::shared_ptr<PlotGroup> getActiveRecorderGroup()
	{
		std::shared_ptr<PlotGroup> group = getActiveGroup();
		return group->getType() == PlotGroup::Type::Recorder ? group : nullptr;
	}

	std::shared_ptr<PlotGroup> getActiveSamplingGroup()
	{
		std::shared_ptr<PlotGroup> group = getActiveGroup();
		return group->getType() == PlotGroup::Type::Sampling ? group : nullptr;
	}

	bool checkIfGroupExists(const std::string& name) const
	{
		return groupMap.find(name) != groupMap.end();
	}

	/* A name built from the stem that no group carries yet: the stem itself
	   while it is free, then the stem with the first free number after it.
	   That is how a new group is named. */
	std::string makeFreeName(const std::string& stem) const
	{
		for (uint32_t index = 0;; index++)
		{
			const std::string candidate = stem + std::to_string(index);

			if (!checkIfGroupExists(candidate))
				return candidate;
		}
	}

	/* A name for a copy of a group, so that a copy is recognisable as one. */
	std::string makeFreeCopyName(const std::string& stem) const
	{
		for (uint32_t index = 0;; index++)
		{
			const std::string candidate = stem + "_copy_" + std::to_string(index);

			if (!checkIfGroupExists(candidate))
				return candidate;
		}
	}

	/* Duplicates a group together with every plot in it and every group nested
	   in it, as a new group of the type asked for, sitting at the top level.
	   The type is asked for rather than taken from the source because the same
	   call serves both "copy this group" and "copy it to the other kind of
	   group". Returns the name the copy was given, or an empty string when
	   there is no group by that name. */
	std::string copyGroup(const std::string& sourceName, PlotGroup::Type type, PlotHandler& plotHandler)
	{
		if (!groupMap.contains(sourceName))
			return "";

		const std::string copyName = makeFreeCopyName(sourceName);
		addGroup(copyName, type);
		copyGroupBelow(groupMap.at(sourceName), copyName, plotHandler);

		return copyName;
	}

   private:
	/* Fills a group that has already been created with copies of the plots of
	   the source and of the groups nested in it. Every group below is created
	   under the copy, so a copied branch does not end up pointing back at the
	   original. */
	void copyGroupBelow(const std::shared_ptr<PlotGroup>& source, const std::string& destinationName, PlotHandler& plotHandler)
	{
		const std::shared_ptr<PlotGroup> destination = groupMap.at(destinationName);

		for (const auto& [plotName, entry] : *source)
		{
			if (entry.plot == nullptr)
				continue;

			destination->addPlot(plotHandler.addPlotCopy(*entry.plot, plotName), entry.visibility);
		}

		/* getChildNames returns a list of its own, so adding groups below while
		   walking it cannot change what is being walked. */
		for (const std::string& childName : getChildNames(source->getName()))
		{
			const std::shared_ptr<PlotGroup> child = groupMap.at(childName);
			const std::string childCopyName = makeFreeCopyName(childName);

			addGroup(childCopyName, child->getType(), destinationName);
			copyGroupBelow(child, childCopyName, plotHandler);
		}
	}

	std::string activeGroup = "";
	std::map<std::string, std::shared_ptr<PlotGroup>> groupMap;
};