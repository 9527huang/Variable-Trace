#include <string>

#include "McpTools.hpp"

namespace mcp
{
	namespace
	{
		Json listGroups(McpContext& context, const Json&)
		{
			if (context.plotGroupHandler == nullptr)
				reportUnavailable("the group list", "list_groups");

			const std::string active = context.plotGroupHandler->getActiveGroupName();
			Json groups = Json::array();

			for (const auto& [name, group] : *context.plotGroupHandler)
				groups.push_back(Json::object({{"name", name},
											   {"type", group->getTypeName()},
											   {"active", name == active},
											   {"parent", group->getParentName()},
											   {"depth", context.plotGroupHandler->getDepth(name)}}));

			return groups;
		}

		Json addGroup(McpContext& context, const Json& arguments)
		{
			if (context.plotGroupHandler == nullptr)
				reportUnavailable("groups", "add_group");

			const std::string name = requireString(arguments, "name");
			const std::string type = optionalString(arguments, "type").value_or("sampling");
			const std::string parent = optionalString(arguments, "parent").value_or("");

			PlotGroup::Type groupType = PlotGroup::Type::Sampling;

			if (type == "recorder")
				groupType = PlotGroup::Type::Recorder;
			else if (type != "sampling")
				throw ToolError("Unknown group type '" + type + "'. Use 'sampling' or 'recorder'.");

			if (context.plotGroupHandler->checkIfGroupExists(name))
				throw ToolError("Group '" + name + "' already exists. Use rename_group or pick another name.");

			if (!parent.empty() && !context.plotGroupHandler->checkIfGroupExists(parent))
				throw ToolError("Group '" + parent + "' does not exist. Use list_groups to see the available groups.");

			/* The group map is walked by the acquisition thread while it works
			   out what to read from the target, so a group cannot be added
			   underneath that walk. */
			{
				auto modelLock = context.variableHandler->lockModel();
				context.plotGroupHandler->addGroup(name, groupType, parent);
			}

			return Json::object({{"name", name}, {"type", type}, {"parent", parent}});
		}

		Json renameGroup(McpContext& context, const Json& arguments)
		{
			const std::string name = requireString(arguments, "name");
			const std::string newName = requireString(arguments, "new_name");
			const std::string resolved = resolveGroupName(context, name);

			if (context.plotGroupHandler->checkIfGroupExists(newName))
				throw ToolError("Group '" + newName + "' already exists.");

			/* A rename moves a node inside a map the acquisition thread walks. */
			{
				auto modelLock = context.variableHandler->lockModel();
				context.plotGroupHandler->renameGroup(resolved, newName);
			}

			return Json::object({{"name", resolved}, {"new_name", newName}});
		}

		Json removeGroup(McpContext& context, const Json& arguments)
		{
			const std::string name = requireString(arguments, "name");
			const std::string resolved = resolveGroupName(context, name);
			/* The removal and the count taken around it belong to the same
			   change, and the map being changed is one the acquisition thread
			   walks on every sample. */
			uint32_t removed = 0;

			{
				auto modelLock = context.variableHandler->lockModel();

				const uint32_t before = static_cast<uint32_t>(context.plotGroupHandler->getGroupCount());

				context.plotGroupHandler->removeGroup(resolved);

				/* Removing a group takes its nested groups with it, so the count
				   that disappeared is reported rather than assumed to be one. */
				removed = before - static_cast<uint32_t>(context.plotGroupHandler->getGroupCount());
			}

			return Json::object({{"removed", resolved}, {"removed_count", removed}, {"active", context.plotGroupHandler->getActiveGroupName()}});
		}

		/* The group copy lives in the group handler, so that the tool and the
		   tree menu duplicate a group the same way - same names, same depth,
		   same plots. What is left here is the counting the answer needs. */
		Json copyGroup(McpContext& context, const Json& arguments)
		{
			if (context.plotGroupHandler == nullptr || context.plotHandler == nullptr)
				reportUnavailable("groups", "copy_group");

			const std::string sourceName = resolveGroupName(context, requireString(arguments, "target"));
			const std::shared_ptr<PlotGroup> source = context.plotGroupHandler->getGroup(sourceName);

			/* A copy adds to both maps, and the acquisition thread walks both. */
			uint32_t groups = 0;
			uint32_t plots = 0;
			std::string newName;

			{
				auto modelLock = context.variableHandler->lockModel();

				const uint32_t groupsBefore = static_cast<uint32_t>(context.plotGroupHandler->getGroupCount());
				const uint32_t plotsBefore = context.plotHandler->getPlotsCount();

				newName = context.plotGroupHandler->copyGroup(sourceName, source->getType(), *context.plotHandler);

				/* A copy carries the groups nested in the source along with it,
				   so the counts are the difference rather than a single group or
				   plot. */
				groups = static_cast<uint32_t>(context.plotGroupHandler->getGroupCount()) - groupsBefore;
				plots = context.plotHandler->getPlotsCount() - plotsBefore;
			}

			if (newName.empty())
				throw ToolError("Group '" + sourceName + "' does not exist. Use list_groups to see the available groups.");

			return Json::object({{"source", sourceName}, {"group", newName}, {"type", source->getTypeName()}, {"plots", plots}, {"groups", groups}});
		}

		Json listVariablesInGroup(McpContext& context, const Json& arguments)
		{
			if (context.plotGroupHandler == nullptr)
				reportUnavailable("groups", "list_variables_in_group");

			const std::string groupName = resolveGroupName(context, optionalString(arguments, "group").value_or(""));
			const std::shared_ptr<PlotGroup> group = context.plotGroupHandler->getGroup(groupName);

			Json plots = Json::array();

			for (const auto& [plotName, entry] : *group)
			{
				Json variables = Json::array();
				std::string type = "unknown";

				if (entry.plot != nullptr)
				{
					type = plotTypeToString(entry.plot->getType());

					for (const auto& [seriesName, series] : entry.plot->getSeriesMap())
					{
						if (series->var != nullptr)
							variables.push_back(series->var->getName());
					}
				}

				plots.push_back(Json::object({{"plot", plotName}, {"type", type}, {"variables", variables}}));
			}

			return Json::object({{"group", groupName}, {"plots", plots}});
		}

		Json setActiveGroup(McpContext& context, const Json& arguments)
		{
			const std::string resolved = resolveGroupName(context, requireString(arguments, "name"));

			context.plotGroupHandler->setActiveGroup(resolved);

			/* Which variables are read from the target follows the active group, so a
			   running acquisition has to rebuild its sample list or it would keep
			   reading the group that is no longer displayed. */
			const bool restarted = requestAcquisitionRestart(context);

			return Json::object({{"active", resolved}, {"acquisition_restarted", restarted}});
		}
	}  // namespace

	void GroupTools::registerAll(ToolRegistry& registry)
	{
		registry.add({.name = "list_groups",
					  .description = "List all groups. Returns a JSON array of objects with fields: 'name' (use this exact string for other group tools), 'type' ('sampling' or 'recorder'), 'active' (bool), 'parent' (name of the group it is nested in, empty for a top-level group) and 'depth' (0 for a top-level group).",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = listGroups});

		registry.add({.name = "add_group",
					  .description = "Add a new group. 'type' must be 'sampling' (default) or 'recorder'. 'parent' nests the new group inside an existing one; omit it for a top-level group.",
					  .inputSchema = objectSchema(Json::object({{"name", stringProperty("Group name")},
																{"type", enumProperty("'sampling' or 'recorder'", {"sampling", "recorder"})},
																{"parent", stringProperty("Name of the group to nest this one in (optional)")}}),
												  {"name"}),
					  .handler = addGroup});

		registry.add({.name = "rename_group",
					  .description = "Rename an existing group.",
					  .inputSchema = objectSchema(Json::object({{"name", stringProperty("Current group name")},
																{"new_name", stringProperty("New group name")}}),
												  {"name", "new_name"}),
					  .handler = renameGroup});

		registry.add({.name = "remove_group",
					  .description = "Remove a group. Groups nested inside it are removed with it; 'removed_count' reports how many went. If it was the last group, a default empty group is created automatically.",
					  .inputSchema = objectSchema(Json::object({{"name", stringProperty("Group name")}}), {"name"}),
					  .handler = removeGroup});

		registry.add({.name = "copy_group",
					  .description = "Copy a group as a new group of the same type. Plots and nested groups are duplicated; the copy gets an auto-generated name. 'plots' is the number of plots copied, 'groups' the number of groups.",
					  .inputSchema = objectSchema(Json::object({{"target", stringProperty("Group to copy")}}), {"target"}),
					  .handler = copyGroup});

		registry.add({.name = "list_variables_in_group",
					  .description = "List all variables assigned to plots within a specific group. Returns a JSON array of {plot, type, variables: [name, ...]}.",
					  .inputSchema = objectSchema(Json::object({{"group", stringProperty("Group name (omit for the active group)")}})),
					  .handler = listVariablesInGroup});

		registry.add({.name = "set_active_group",
					  .description = "Set the active group. If acquisition is running and 'restart on group change' is enabled, acquisition restarts automatically.",
					  .inputSchema = objectSchema(Json::object({{"name", stringProperty("Group name")}}), {"name"}),
					  .handler = setActiveGroup});
	}
}  // namespace mcp
