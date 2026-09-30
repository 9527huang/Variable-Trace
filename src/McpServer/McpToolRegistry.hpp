#ifndef _MCPTOOLREGISTRY_HPP
#define _MCPTOOLREGISTRY_HPP

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "McpContext.hpp"
#include "McpTypes.hpp"

namespace mcp
{
	using ToolHandler = std::function<Json(McpContext& context, const Json& arguments)>;

	struct Tool
	{
		std::string name;
		std::string description;
		Json inputSchema = Json::object({});
		ToolHandler handler;

		/*
		 * Tools that read or change the variable and plot model have to run on the
		 * GUI thread. Tools that only touch files, or that start a parser process,
		 * run on the request thread so that a slow operation does not freeze the
		 * application. True is the safe default.
		 */
		bool onGuiThread = true;
	};

	class ToolRegistry
	{
	   public:
		/* Duplicate names are refused: the client addresses tools by name, so a
		   second registration would silently make one of them unreachable. */
		bool add(Tool tool);
		const Tool* find(const std::string& name) const;
		size_t count() const;

		/* Payload of tools/list, in registration order. */
		Json listTools() const;

		std::vector<std::string> getNames() const;

		/* The registered tools themselves, so that the table can be inspected
		   without going through the wire format. */
		const std::vector<Tool>& getTools() const
		{
			return tools;
		}

	   private:
		std::vector<Tool> tools;
		std::map<std::string, size_t> indexByName;
	};

	/* The schema builders below keep the registration files readable. Every tool
	   describes its arguments as a JSON Schema object, which is what clients show
	   to the model. */
	Json objectSchema(Json properties, std::vector<std::string> required = {});
	Json stringProperty(const std::string& description);
	Json numberProperty(const std::string& description);
	Json integerProperty(const std::string& description);
	Json booleanProperty(const std::string& description);
	Json enumProperty(const std::string& description, const std::vector<std::string>& values);
	Json arrayProperty(const std::string& description, Json items);
}  // namespace mcp

#endif
