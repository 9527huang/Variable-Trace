#include "McpToolRegistry.hpp"

namespace mcp
{
	bool ToolRegistry::add(Tool tool)
	{
		if (tool.name.empty() || !tool.handler)
			return false;

		if (indexByName.contains(tool.name))
			return false;

		indexByName.emplace(tool.name, tools.size());
		tools.push_back(std::move(tool));

		return true;
	}

	const Tool* ToolRegistry::find(const std::string& name) const
	{
		auto entry = indexByName.find(name);

		if (entry == indexByName.end())
			return nullptr;

		return &tools.at(entry->second);
	}

	size_t ToolRegistry::count() const
	{
		return tools.size();
	}

	Json ToolRegistry::listTools() const
	{
		Json list = Json::array();

		for (const Tool& tool : tools)
		{
			list.push_back(Json::object({{"name", tool.name},
										 {"description", tool.description},
										 {"inputSchema", tool.inputSchema}}));
		}

		return Json::object({{"tools", list}});
	}

	std::vector<std::string> ToolRegistry::getNames() const
	{
		std::vector<std::string> names;
		names.reserve(tools.size());

		for (const Tool& tool : tools)
			names.push_back(tool.name);

		return names;
	}

	Json objectSchema(Json properties, std::vector<std::string> required)
	{
		Json schema = Json::object({{"type", "object"}, {"properties", std::move(properties)}});

		if (!required.empty())
			schema["required"] = required;

		return schema;
	}

	Json stringProperty(const std::string& description)
	{
		return Json::object({{"type", "string"}, {"description", description}});
	}

	Json numberProperty(const std::string& description)
	{
		return Json::object({{"type", "number"}, {"description", description}});
	}

	Json integerProperty(const std::string& description)
	{
		return Json::object({{"type", "integer"}, {"description", description}});
	}

	Json booleanProperty(const std::string& description)
	{
		return Json::object({{"type", "boolean"}, {"description", description}});
	}

	Json enumProperty(const std::string& description, const std::vector<std::string>& values)
	{
		return Json::object({{"type", "string"}, {"description", description}, {"enum", values}});
	}

	Json arrayProperty(const std::string& description, Json items)
	{
		return Json::object({{"type", "array"}, {"description", description}, {"items", std::move(items)}});
	}
}  // namespace mcp
