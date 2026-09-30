#include "GlobalConfig.hpp"
#include "McpTools.hpp"

namespace mcp
{
	namespace
	{
		Json newProject(McpContext& context, const Json&)
		{
			if (!context.newProject)
				reportUnavailable("starting a new project", "new_project");

			context.newProject();

			return Json::object({{"new", true}});
		}

		Json openProject(McpContext& context, const Json& arguments)
		{
			if (!context.openProject)
				reportUnavailable("opening a project", "open_project");

			const std::optional<std::string> requested = optionalString(arguments, "path");
			const std::string path = requested.value_or("");

			/* The picker runs a modal dialog, which is why this tool stays on the GUI
			   thread while the request thread waits in the bridge. */
			if (!context.openProject(path))
				throw ToolError(path.empty() ? "No project was selected." : "Project '" + path + "' could not be opened.");

			const std::string resolved = path.empty() && context.getProjectPath ? context.getProjectPath() : path;

			return Json::object({{"path", resolved}, {"opened", true}});
		}

		Json openRecentProject(McpContext& context, const Json& arguments)
		{
			if (!context.openProject)
				reportUnavailable("opening a project", "open_recent_project");

			const int64_t index = optionalInteger(arguments, "index").value_or(-1);

			if (index < 0)
				throw ToolError("Field 'index' is required.");

			if (context.globalConfig == nullptr)
				throw ToolError("The recent project list is not available.");

			const std::vector<std::string> recent = context.globalConfig->getSettings().recentProjects;

			if (static_cast<size_t>(index) >= recent.size())
				throw ToolError("Index " + std::to_string(index) + " is outside the recent project list, which holds " + std::to_string(recent.size()) + " entr" + (recent.size() == 1 ? "y" : "ies") + ". Call list_recent_projects first.");

			const std::string path = recent.at(static_cast<size_t>(index));

			if (!context.openProject(path))
				throw ToolError("Project '" + path + "' could not be opened.");

			return Json::object({{"index", index}, {"path", path}, {"opened", true}});
		}

		Json listRecentProjects(McpContext& context, const Json&)
		{
			if (context.globalConfig == nullptr)
				reportUnavailable("the recent project list", "list_recent_projects");

			return context.globalConfig->getSettings().recentProjects;
		}

		Json saveProject(McpContext& context, const Json&)
		{
			if (!context.saveProject)
				reportUnavailable("saving a project", "save_project");

			const std::string path = context.getProjectPath ? context.getProjectPath() : "";

			if (path.empty())
				throw ToolError("The project has no file yet. Use save_project_as first.");

			if (!context.saveProject())
				throw ToolError("Project '" + path + "' could not be saved.");

			return Json::object({{"path", path}, {"saved", true}});
		}

		Json saveProjectAs(McpContext& context, const Json& arguments)
		{
			if (!context.saveProjectAs)
				reportUnavailable("saving a project", "save_project_as");

			const std::string path = optionalString(arguments, "path").value_or("");

			if (!context.saveProjectAs(path))
				throw ToolError(path.empty() ? "No target file was selected." : "Project could not be saved to '" + path + "'.");

			return Json::object({{"path", context.getProjectPath ? context.getProjectPath() : path}, {"saved", true}});
		}
	}  // namespace

	void ProjectTools::registerAll(ToolRegistry& registry)
	{
		registry.add({.name = "new_project",
					  .description = "Create a new empty project, resetting all current state.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = newProject});

		registry.add({.name = "open_project",
					  .description = "Open a project file. Omit 'path' to use the file picker.",
					  .inputSchema = objectSchema(Json::object({{"path", stringProperty("Absolute path to the .mcvproj file")}})),
					  .handler = openProject});

		registry.add({.name = "open_recent_project",
					  .description = "Open a project by its 0-based index in the recent list (see list_recent_projects).",
					  .inputSchema = objectSchema(Json::object({{"index", integerProperty("Zero-based index into the recent projects list")}}), {"index"}),
					  .handler = openRecentProject});

		registry.add({.name = "list_recent_projects",
					  .description = "Return a JSON array of recently opened project paths. Use the array index with open_recent_project.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = listRecentProjects});

		registry.add({.name = "save_project",
					  .description = "Save the currently open project to its existing file.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = saveProject});

		registry.add({.name = "save_project_as",
					  .description = "Save the project to a new file. Omit 'path' to use the file picker.",
					  .inputSchema = objectSchema(Json::object({{"path", stringProperty("Absolute path for the new .mcvproj file")}})),
					  .handler = saveProjectAs});
	}
}  // namespace mcp
