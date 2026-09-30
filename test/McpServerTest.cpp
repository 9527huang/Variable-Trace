#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "McpBridge.hpp"
#include "McpContext.hpp"
#include "McpProtocol.hpp"
#include "McpServer.hpp"
#include "McpToolRegistry.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "PlotGroupHandler.hpp"
#include "PlotHandler.hpp"
#include "VariableHandler.hpp"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/spdlog.h"

/*
 * The HTTP library is deliberately not included here. It is large enough that
 * adding it to this file pushes the object past the section count a COFF object
 * can address, and the link then fails on symbols this very file defines. The
 * requests over a real socket are checked by build/mcp_e2e.py instead, which
 * starts the application and speaks to it over the loopback interface.
 */

/*
 * The tests below cover the part of the API server that can be exercised without
 * a window, a socket or hardware: the argument parsing, the tool registry and the
 * JSON-RPC layer. The tool handlers themselves reach into the application model
 * and are covered by the manual end to end run.
 */
namespace
{
	using mcp::Json;

	const Json& contentText(const Json& envelope)
	{
		return envelope.at("content").at(0).at("text");
	}

	std::string textOf(const Json& envelope)
	{
		return contentText(envelope).get<std::string>();
	}

	/* An invoker that answers every call with a fixed envelope, so the protocol
	   layer can be tested on its own. */
	mcp::Protocol makeProtocol(const mcp::ToolRegistry& registry)
	{
		return mcp::Protocol(&registry,
							 [](const std::string& name, const Json& arguments)
							 {
								 if (name == "fails")
									 return mcp::toolFailure("the tool could not do its job");

								 return mcp::toolSuccess(Json::object({{"echo", name}, {"arguments", arguments}}));
							 },
							 []()
							 { return std::string("9.9.9"); });
	}

	class ArgumentParsingTest : public ::testing::Test
	{
	};

	TEST_F(ArgumentParsingTest, MissingArgumentsBecomeAnEmptyObject)
	{
		EXPECT_TRUE(mcp::argumentsOf(Json::object({})).empty());
		EXPECT_TRUE(mcp::argumentsOf(Json::object({{"arguments", nullptr}})).empty());
	}

	TEST_F(ArgumentParsingTest, ArgumentsThatAreNotAnObjectAreRefused)
	{
		EXPECT_THROW(mcp::argumentsOf(Json::object({{"arguments", 5}})), mcp::JsonRpcError);
	}

	TEST_F(ArgumentParsingTest, ArgumentsObjectIsReturnedAsIs)
	{
		const Json arguments = mcp::argumentsOf(Json::object({{"arguments", Json::object({{"name", "speed"}})}}));

		ASSERT_TRUE(arguments.is_object());
		EXPECT_EQ(arguments.at("name").get<std::string>(), "speed");
	}

	TEST_F(ArgumentParsingTest, HasTreatsNullAsAbsent)
	{
		const Json arguments = Json::object({{"a", nullptr}, {"b", 1}});

		EXPECT_FALSE(mcp::has(arguments, "a"));
		EXPECT_FALSE(mcp::has(arguments, "missing"));
		EXPECT_TRUE(mcp::has(arguments, "b"));
	}

	TEST_F(ArgumentParsingTest, RequiredStringNamesTheMissingField)
	{
		try
		{
			mcp::requireString(Json::object({}), "name");
			FAIL() << "expected a ToolError";
		}
		catch (const mcp::ToolError& error)
		{
			EXPECT_NE(std::string(error.what()).find("name"), std::string::npos);
		}
	}

	TEST_F(ArgumentParsingTest, RequiredStringRefusesAnEmptyValue)
	{
		EXPECT_THROW(mcp::requireString(Json::object({{"name", ""}}), "name"), mcp::ToolError);
		EXPECT_THROW(mcp::requireString(Json::object({{"name", 12}}), "name"), mcp::ToolError);
	}

	TEST_F(ArgumentParsingTest, OptionalNumberTakesStringsAsWell)
	{
		EXPECT_EQ(*mcp::optionalNumber(Json::object({{"v", 2.5}}), "v"), 2.5);
		EXPECT_EQ(*mcp::optionalNumber(Json::object({{"v", "2.5"}}), "v"), 2.5);
		EXPECT_THROW(mcp::optionalNumber(Json::object({{"v", "abc"}}), "v"), mcp::ToolError);
		EXPECT_FALSE(mcp::optionalNumber(Json::object({}), "v").has_value());
	}

	TEST_F(ArgumentParsingTest, OptionalBoolTakesZeroAndOne)
	{
		EXPECT_TRUE(*mcp::optionalBool(Json::object({{"v", true}}), "v"));
		EXPECT_FALSE(*mcp::optionalBool(Json::object({{"v", 0}}), "v"));
		EXPECT_TRUE(*mcp::optionalBool(Json::object({{"v", 1}}), "v"));
		EXPECT_TRUE(*mcp::optionalBool(Json::object({{"v", "true"}}), "v"));
		EXPECT_FALSE(*mcp::optionalBool(Json::object({{"v", "false"}}), "v"));
		EXPECT_THROW(mcp::optionalBool(Json::object({{"v", "yes"}}), "v"), mcp::ToolError);
	}

	TEST_F(ArgumentParsingTest, OptionalUnsignedReadsHexadecimalMasks)
	{
		EXPECT_EQ(*mcp::optionalUnsigned(Json::object({{"mask", 4294967295ull}}), "mask"), 4294967295ull);
		EXPECT_EQ(*mcp::optionalUnsigned(Json::object({{"mask", "0xFFFFFFFF"}}), "mask"), 4294967295ull);
		EXPECT_EQ(*mcp::optionalUnsigned(Json::object({{"mask", "65535"}}), "mask"), 65535ull);

		/* A value with trailing text must not be read as its prefix, that would
		   apply a mask nobody asked for. */
		EXPECT_THROW(mcp::optionalUnsigned(Json::object({{"mask", "12abc"}}), "mask"), mcp::ToolError);
		EXPECT_THROW(mcp::optionalUnsigned(Json::object({{"mask", -1}}), "mask"), mcp::ToolError);
	}

	TEST_F(ArgumentParsingTest, StringArrayAcceptsASingleNameAsWell)
	{
		const Json single = Json::object({{"variables", "a"}});
		const Json several = Json::object({{"variables", Json::array({"a", "b"})}});

		EXPECT_EQ(mcp::stringArray(single.at("variables"), "variables"), std::vector<std::string>{"a"});
		EXPECT_EQ(mcp::stringArray(several.at("variables"), "variables"), (std::vector<std::string>{"a", "b"}));

		EXPECT_THROW(mcp::stringArray(Json::array(), "variables"), mcp::ToolError);
		EXPECT_THROW(mcp::stringArray(Json::array({1}), "variables"), mcp::ToolError);
	}

	class ToolRegistryTest : public ::testing::Test
	{
	   protected:
		mcp::Tool makeTool(const std::string& name)
		{
			return mcp::Tool{.name = name,
							 .description = "does something",
							 .inputSchema = mcp::objectSchema(Json::object({})),
							 .handler = [](McpContext&, const Json&)
							 { return Json::object({}); }};
		}

		mcp::ToolRegistry registry;
	};

	TEST_F(ToolRegistryTest, ToolsAreFoundByName)
	{
		ASSERT_TRUE(registry.add(makeTool("alpha")));
		ASSERT_TRUE(registry.add(makeTool("beta")));

		EXPECT_EQ(registry.count(), 2u);
		ASSERT_NE(registry.find("alpha"), nullptr);
		EXPECT_EQ(registry.find("alpha")->description, "does something");
		EXPECT_EQ(registry.find("missing"), nullptr);
	}

	TEST_F(ToolRegistryTest, ADuplicateNameIsRefused)
	{
		ASSERT_TRUE(registry.add(makeTool("alpha")));

		/* A second registration would make one of the two unreachable, since the
		   client addresses tools by name. */
		EXPECT_FALSE(registry.add(makeTool("alpha")));
		EXPECT_EQ(registry.count(), 1u);
	}

	TEST_F(ToolRegistryTest, IncompleteToolsAreRefused)
	{
		mcp::Tool withoutHandler = makeTool("alpha");
		withoutHandler.handler = nullptr;

		EXPECT_FALSE(registry.add(withoutHandler));
		EXPECT_FALSE(registry.add(mcp::Tool{}));
		EXPECT_EQ(registry.count(), 0u);
	}

	TEST_F(ToolRegistryTest, ListPayloadCarriesNameDescriptionAndSchema)
	{
		ASSERT_TRUE(registry.add(makeTool("alpha")));

		const Json list = registry.listTools();

		ASSERT_TRUE(list.contains("tools"));
		ASSERT_EQ(list.at("tools").size(), 1u);
		EXPECT_EQ(list.at("tools").at(0).at("name").get<std::string>(), "alpha");
		EXPECT_TRUE(list.at("tools").at(0).at("description").is_string());
		EXPECT_EQ(list.at("tools").at(0).at("inputSchema").at("type").get<std::string>(), "object");
	}

	TEST_F(ToolRegistryTest, RegistrationOrderIsKept)
	{
		ASSERT_TRUE(registry.add(makeTool("gamma")));
		ASSERT_TRUE(registry.add(makeTool("alpha")));

		const std::vector<std::string> names = registry.getNames();

		EXPECT_EQ(names, (std::vector<std::string>{"gamma", "alpha"}));
	}

	TEST_F(ToolRegistryTest, SchemaBuildersDescribeTheirValues)
	{
		const Json schema = mcp::objectSchema(Json::object({{"mode", mcp::enumProperty("pick one", {"a", "b"})},
															{"items", mcp::arrayProperty("many", mcp::stringProperty("one"))}}),
											  {"mode"});

		EXPECT_EQ(schema.at("properties").at("mode").at("enum").size(), 2u);
		ASSERT_TRUE(schema.contains("required"));
		EXPECT_EQ(schema.at("required").at(0).get<std::string>(), "mode");
		EXPECT_EQ(schema.at("properties").at("items").at("items").at("type").get<std::string>(), "string");

		/* Without required fields the key is left out rather than sent empty. */
		EXPECT_FALSE(mcp::objectSchema(Json::object({})).contains("required"));
	}

	class ProtocolTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			registry.add(mcp::Tool{.name = "echo",
								   .description = "echoes",
								   .inputSchema = mcp::objectSchema(Json::object({})),
								   .handler = [](McpContext&, const Json& arguments)
								   { return arguments; }});

			registry.add(mcp::Tool{.name = "fails",
								   .description = "always fails",
								   .inputSchema = mcp::objectSchema(Json::object({})),
								   .handler = [](McpContext&, const Json&) -> Json
								   { throw mcp::ToolError("no"); }});

			protocol = std::make_unique<mcp::Protocol>(makeProtocol(registry));
		}

		mcp::ToolRegistry registry;
		std::unique_ptr<mcp::Protocol> protocol;
	};

	TEST_F(ProtocolTest, InitializeEchoesASupportedProtocolVersion)
	{
		const Json reply = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"},
																 {"id", 1},
																 {"method", "initialize"},
																 {"params", Json::object({{"protocolVersion", "2025-03-26"}})}}));

		ASSERT_TRUE(reply.contains("result"));
		const Json& result = reply.at("result");

		EXPECT_EQ(result.at("protocolVersion").get<std::string>(), "2025-03-26");
		EXPECT_EQ(result.at("serverInfo").at("name").get<std::string>(), mcp::serverName);
		EXPECT_EQ(result.at("serverInfo").at("version").get<std::string>(), "9.9.9");
		EXPECT_FALSE(result.at("capabilities").at("tools").at("listChanged").get<bool>());
		EXPECT_FALSE(result.at("instructions").get<std::string>().empty());
	}

	TEST_F(ProtocolTest, UnknownProtocolVersionFallsBackToTheNewest)
	{
		const Json reply = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"},
																 {"id", 1},
																 {"method", "initialize"},
																 {"params", Json::object({{"protocolVersion", "1999-01-01"}})}}));

		EXPECT_EQ(reply.at("result").at("protocolVersion").get<std::string>(), mcp::latestProtocolVersion);
	}

	TEST_F(ProtocolTest, InitializeWithoutParamsUsesTheNewestVersion)
	{
		const Json reply = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}}));

		EXPECT_EQ(reply.at("result").at("protocolVersion").get<std::string>(), mcp::latestProtocolVersion);
	}

	TEST_F(ProtocolTest, PingIsAnsweredWithAnEmptyResult)
	{
		const Json reply = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}}));

		EXPECT_EQ(reply.at("id").get<int>(), 7);
		EXPECT_TRUE(reply.at("result").empty());
	}

	TEST_F(ProtocolTest, ToolsListReturnsTheRegistry)
	{
		const Json reply = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}));

		EXPECT_EQ(reply.at("result").at("tools").size(), 2u);
	}

	TEST_F(ProtocolTest, AToolCallReturnsThePayloadInTextContent)
	{
		const Json reply = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"},
																 {"id", 3},
																 {"method", "tools/call"},
																 {"params", Json::object({{"name", "echo"},
																						  {"arguments", Json::object({{"value", 4}})}})}}));

		const Json& result = reply.at("result");

		ASSERT_TRUE(result.contains("isError"));
		EXPECT_FALSE(result.at("isError").get<bool>());
		EXPECT_EQ(result.at("content").at(0).at("type").get<std::string>(), "text");

		const Json payload = Json::parse(textOf(result));

		EXPECT_EQ(payload.at("echo").get<std::string>(), "echo");
		EXPECT_EQ(payload.at("arguments").at("value").get<int>(), 4);
	}

	TEST_F(ProtocolTest, AToolFailureIsReportedWithIsError)
	{
		/* A tool that could not do its job is a successful call whose payload says
		   so, because that is what a model can read and react to. */
		const Json reply = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"},
																 {"id", 4},
																 {"method", "tools/call"},
																 {"params", Json::object({{"name", "fails"}})}}));

		ASSERT_FALSE(reply.contains("error"));
		EXPECT_TRUE(reply.at("result").at("isError").get<bool>());
		EXPECT_EQ(textOf(reply.at("result")), "the tool could not do its job");
	}

	TEST_F(ProtocolTest, ACallWithoutAToolNameIsAProtocolError)
	{
		const Json reply = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"},
																 {"id", 5},
																 {"method", "tools/call"},
																 {"params", Json::object({})}}));

		ASSERT_TRUE(reply.contains("error"));
		EXPECT_EQ(reply.at("error").at("code").get<int>(), mcp::JsonRpcError::invalidParams);
	}

	TEST_F(ProtocolTest, UnknownMethodsAreReportedAsSuch)
	{
		const Json reply = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", 6}, {"method", "does/not/exist"}}));

		ASSERT_TRUE(reply.contains("error"));
		EXPECT_EQ(reply.at("error").at("code").get<int>(), mcp::JsonRpcError::methodNotFound);
	}

	TEST_F(ProtocolTest, ListMethodsWithoutDataAnswerWithEmptyLists)
	{
		for (const std::string& method : {std::string("resources/list"), std::string("prompts/list")})
		{
			const Json reply = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", 8}, {"method", method}}));

			ASSERT_TRUE(reply.contains("result"));
			EXPECT_TRUE(reply.at("result").is_object());
		}

		const Json logging = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", 9}, {"method", "logging/setLevel"}, {"params", Json::object({{"level", "debug"}})}}));

		EXPECT_TRUE(logging.at("result").empty());
	}

	TEST_F(ProtocolTest, NotificationsAreNeverAnswered)
	{
		EXPECT_TRUE(protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}})).is_null());

		/* Even a notification whose handling fails must stay unanswered. */
		EXPECT_TRUE(protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"method", "tools/call"}, {"params", Json::object({})}})).is_null());
		EXPECT_TRUE(protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"method", "unknown/method"}})).is_null());
	}

	TEST_F(ProtocolTest, AnExplicitNullIdIsTreatedAsANotification)
	{
		EXPECT_TRUE(protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", nullptr}, {"method", "ping"}})).is_null());
	}

	TEST_F(ProtocolTest, AReplyToTheServerIsIgnored)
	{
		EXPECT_TRUE(protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", 10}, {"result", Json::object({})}})).is_null());
		EXPECT_TRUE(protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", 11}, {"error", Json::object({{"code", -1}})}})).is_null());
	}

	TEST_F(ProtocolTest, AMessageWithoutMethodOrResultIsRejected)
	{
		const Json reply = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", 12}}));

		ASSERT_TRUE(reply.contains("error"));
		EXPECT_EQ(reply.at("error").at("code").get<int>(), mcp::JsonRpcError::invalidRequest);
	}

	TEST_F(ProtocolTest, BatchedRequestsAreRejected)
	{
		const Json reply = protocol->handleMessage(Json::array({Json::object({{"jsonrpc", "2.0"}, {"id", 13}, {"method", "ping"}})}));

		ASSERT_TRUE(reply.contains("error"));
		EXPECT_EQ(reply.at("error").at("code").get<int>(), mcp::JsonRpcError::invalidRequest);
	}

	TEST_F(ProtocolTest, AMessageThatIsNotAnObjectIsRejected)
	{
		const Json reply = protocol->handleMessage(Json(5));

		ASSERT_TRUE(reply.contains("error"));
		EXPECT_EQ(reply.at("error").at("code").get<int>(), mcp::JsonRpcError::invalidRequest);
	}

	TEST_F(ProtocolTest, AStringIdIsPreserved)
	{
		const Json reply = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", "abc"}, {"method", "ping"}}));

		EXPECT_EQ(reply.at("id").get<std::string>(), "abc");
	}

	TEST_F(ProtocolTest, ParamsThatAreNotAnObjectAreIgnored)
	{
		const Json reply = protocol->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", 14}, {"method", "initialize"}, {"params", "nonsense"}}));

		ASSERT_TRUE(reply.contains("result"));
		EXPECT_EQ(reply.at("result").at("protocolVersion").get<std::string>(), mcp::latestProtocolVersion);
	}

	TEST_F(ProtocolTest, ErrorAndResultEnvelopesCarryTheRevision)
	{
		const Json error = mcp::Protocol::wrappedError(Json(1), mcp::JsonRpcError::internalError, "boom");
		const Json result = mcp::Protocol::wrappedResult(Json(1), Json::object({{"a", 1}}));

		EXPECT_EQ(error.at("jsonrpc").get<std::string>(), "2.0");
		EXPECT_EQ(error.at("error").at("message").get<std::string>(), "boom");
		EXPECT_EQ(result.at("jsonrpc").get<std::string>(), "2.0");
		EXPECT_EQ(result.at("result").at("a").get<int>(), 1);
	}

	class ToolHelpersTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			context.variableHandler = &variableHandler;
			context.plotHandler = &plotHandler;
			context.plotGroupHandler = &plotGroupHandler;

			plotGroupHandler.addGroup("first");
			plotGroupHandler.addGroup("second");
			plotGroupHandler.setActiveGroup("second");

			variableHandler.addVariable(std::make_shared<Variable>("speed"));
		}

		VariableHandler variableHandler;
		PlotHandler plotHandler;
		PlotGroupHandler plotGroupHandler;
		McpContext context;
	};

	TEST_F(ToolHelpersTest, VariableTypesMapBothWays)
	{
		EXPECT_EQ(mcp::variableTypeFromString("u32"), Variable::Type::U32);
		EXPECT_EQ(mcp::variableTypeFromString("f32"), Variable::Type::F32);
		EXPECT_EQ(mcp::variableTypeToString(Variable::Type::I16), "i16");

		try
		{
			mcp::variableTypeFromString("u64");
			FAIL() << "expected a ToolError";
		}
		catch (const mcp::ToolError& error)
		{
			/* The message has to list the accepted spellings, a model only has the
			   text of the failure to correct itself with. */
			EXPECT_NE(std::string(error.what()).find("u32"), std::string::npos);
		}
	}

	TEST_F(ToolHelpersTest, PlotTypesMapBothWays)
	{
		EXPECT_EQ(mcp::plotTypeFromString("curve"), Plot::Type::CURVE);
		EXPECT_EQ(mcp::plotTypeFromString("xy"), Plot::Type::XY);
		EXPECT_EQ(mcp::plotTypeToString(Plot::Type::TABLE), "table");
		EXPECT_THROW(mcp::plotTypeFromString("pie"), mcp::ToolError);
	}

	TEST_F(ToolHelpersTest, EnumInterpretationsMapBothWays)
	{
		EXPECT_EQ(mcp::interpretationFromString("none"), Variable::HighLevelType::NONE);
		EXPECT_EQ(mcp::interpretationFromString("signed_frac"), Variable::HighLevelType::SIGNEDFRAC);
		EXPECT_EQ(mcp::interpretationFromString("enum"), Variable::HighLevelType::ENUM);
		EXPECT_EQ(mcp::interpretationFromString("custom_enum"), Variable::HighLevelType::CUSTOM_ENUM);

		EXPECT_EQ(mcp::interpretationToString(Variable::HighLevelType::UNSIGNEDFRAC), "unsigned_frac");
		EXPECT_EQ(mcp::interpretationToString(Variable::HighLevelType::CUSTOM_ENUM), "custom_enum");

		/* An unknown spelling has to come back with the accepted ones, because
		   the text of the failure is all a caller has to correct itself with. */
		try
		{
			mcp::interpretationFromString("fraction");
			FAIL() << "expected a ToolError";
		}
		catch (const mcp::ToolError& error)
		{
			EXPECT_NE(std::string(error.what()).find("custom_enum"), std::string::npos);
			EXPECT_NE(std::string(error.what()).find("unsigned_frac"), std::string::npos);
		}
	}

	TEST_F(ToolHelpersTest, EnumLabelsRoundTripAndAreChecked)
	{
		const Json labels = Json::array({{{"label", "idle"}, {"value", 0}}, {{"label", "run"}, {"value", 4}}});

		const std::vector<Variable::EnumLabel> parsed = mcp::enumLabelsFromJson(labels, "enum_labels");
		ASSERT_EQ(parsed.size(), 2u);
		EXPECT_EQ(parsed[0].label, "idle");
		EXPECT_EQ(parsed[1].value, 4);

		EXPECT_EQ(mcp::enumLabelsToJson(parsed), labels);

		/* A label is the way back from a number to a name, so an empty one has
		   nothing to say and a repeated one is ambiguous. */
		EXPECT_THROW(mcp::enumLabelsFromJson(Json::array({{{"label", ""}, {"value", 1}}}), "enum_labels"), mcp::ToolError);
		EXPECT_THROW(mcp::enumLabelsFromJson(Json::array({{{"label", "a"}, {"value", 1}}, {{"label", "a"}, {"value", 2}}}), "enum_labels"), mcp::ToolError);
		EXPECT_THROW(mcp::enumLabelsFromJson(Json::array({{{"label", "a"}, {"value", 1.5}}}), "enum_labels"), mcp::ToolError);
		EXPECT_THROW(mcp::enumLabelsFromJson(Json::object({}), "enum_labels"), mcp::ToolError);
	}

	TEST_F(ToolHelpersTest, RequireVariableNamesWhatIsMissing)
	{
		EXPECT_EQ(mcp::requireVariable(context, "speed")->getName(), "speed");

		try
		{
			mcp::requireVariable(context, "torque");
			FAIL() << "expected a ToolError";
		}
		catch (const mcp::ToolError& error)
		{
			EXPECT_NE(std::string(error.what()).find("torque"), std::string::npos);
			EXPECT_NE(std::string(error.what()).find("add_variable"), std::string::npos);
		}
	}

	TEST_F(ToolHelpersTest, AnEmptyGroupNameResolvesToTheActiveGroup)
	{
		EXPECT_EQ(mcp::resolveGroupName(context, ""), "second");
		EXPECT_EQ(mcp::resolveGroupName(context, "first"), "first");
		EXPECT_THROW(mcp::resolveGroupName(context, "third"), mcp::ToolError);
	}

	TEST_F(ToolHelpersTest, AnUnsampledVariableIsReportedAsSuch)
	{
		EXPECT_FALSE(mcp::isVariableSampled(context, "speed"));
		EXPECT_FALSE(mcp::isVariableSampled(context, "missing"));

		variableHandler.getVariable("speed")->setIsCurrentlySampled(true);
		EXPECT_TRUE(mcp::isVariableSampled(context, "speed"));
	}

	TEST_F(ToolHelpersTest, TheNotSampledHintSaysHowToFixIt)
	{
		const std::string hint = mcp::notSampledHint("speed");

		EXPECT_NE(hint.find("speed"), std::string::npos);
		EXPECT_NE(hint.find("add_variable_to_plot"), std::string::npos);
	}

	TEST_F(ToolHelpersTest, UnavailableFeaturesPointAtTheBuildInformation)
	{
		try
		{
			mcp::reportUnavailable("the recorder subsystem", "get_recorder_settings");
			FAIL() << "expected a ToolError";
		}
		catch (const mcp::ToolError& error)
		{
			const std::string message = error.what();

			EXPECT_NE(message.find("get_recorder_settings"), std::string::npos);
			EXPECT_NE(message.find("recorder"), std::string::npos);
			EXPECT_NE(message.find("get_instance_info"), std::string::npos);
		}
	}

	TEST_F(ToolHelpersTest, DecimationKeepsTheEndsAndTheCount)
	{
		/* Fewer samples than asked for: everything is kept. */
		std::vector<size_t> all = mcp::decimateIndices(5, 10);
		EXPECT_EQ(all, (std::vector<size_t>{0, 1, 2, 3, 4}));

		/* Nothing to decimate. */
		EXPECT_TRUE(mcp::decimateIndices(0, 10).empty());

		/* The newest sample is the interesting one, so it must survive. */
		const std::vector<size_t> decimated = mcp::decimateIndices(1000, 10);
		ASSERT_EQ(decimated.size(), 10u);
		EXPECT_EQ(decimated.front(), 0u);
		EXPECT_EQ(decimated.back(), 999u);

		for (size_t index = 1; index < decimated.size(); index++)
			EXPECT_LT(decimated[index - 1], decimated[index]);

		/* An exact count keeps everything. */
		EXPECT_EQ(mcp::decimateIndices(10, 10).size(), 10u);
	}

	/*
	 * The server object itself, without a socket. Constructing it builds the
	 * whole tool table, so this is where the contract with an agent is pinned
	 * down: the names, the schemas, and how a badly formed call is answered.
	 */
	class ServerTest : public testing::Test
	{
	   protected:
		void SetUp() override
		{
			logger = std::make_shared<spdlog::logger>("mcp-test", std::make_shared<spdlog::sinks::null_sink_mt>());
			server = std::make_unique<mcp::Server>(&context, &bridge, logger.get());
		}

		void TearDown() override
		{
			server.reset();
			spdlog::drop_all();
		}

		const std::vector<std::string> names() const
		{
			return server->getRegistry().getNames();
		}

		/*
		 * Runs a call and plays the part of the interface thread while it is in
		 * flight: the tools that touch the variable and plot model are queued by
		 * the bridge, so the queue is drained here.
		 */
		Json callFromTheInterfaceThread(const std::string& name, const Json& arguments)
		{
			Json answer;
			std::atomic<bool> finished{false};

			std::thread caller(
				[this, &answer, &finished, &name, &arguments]()
				{
					answer = server->callTool(name, arguments);
					finished = true;
				});

			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

			while (!finished.load() && std::chrono::steady_clock::now() < deadline)
			{
				bridge.processPendingTasks();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			if (!finished.load())
				bridge.cancelWaitingTasks();

			caller.join();

			return answer;
		}

		/* Declared before the server so that the server is destroyed first, and
		   without any subsystem attached: a tool that reaches into the
		   application stops at its first line and reports the missing part. */
		McpContext context;
		mcp::Bridge bridge;
		std::shared_ptr<spdlog::logger> logger;
		std::unique_ptr<mcp::Server> server;
	};

	TEST_F(ServerTest, TheToolTableMatchesTheDocumentedDomains)
	{
		/* The domains and their sizes come from the reference build, and an agent
		   prompt written against it addresses the tools by these names. */
		const std::vector<std::pair<std::string, std::vector<std::string>>> domains = {
			{"project",
			 {"new_project", "open_project", "open_recent_project", "list_recent_projects", "save_project", "save_project_as"}},
			{"group", {"list_groups", "add_group", "rename_group", "remove_group", "copy_group", "list_variables_in_group", "set_active_group"}},
			{"plot", {"add_plot", "rename_plot", "delete_plot", "add_variable_to_plot"}},
			{"variable",
			 {"add_variable", "remove_variable", "configure_variable", "refresh_variable_addresses", "set_elf_path", "set_sampling",
			  "set_elf_parser", "set_logging"}},
			{"recorder", {"set_recorder_enabled", "detect_recorder", "get_recorder_settings", "set_recorder_mode", "set_recorder_trigger",
						  "set_recorder_downsampling"}},
			{"acquisition", {"set_debug_probe", "configure_probe", "start_acquisition", "stop_acquisition", "get_acquisition_state"}},
			{"data", {"get_variable_value", "get_variable_stats", "set_variable_value", "get_curve_data"}},
			{"flashing", {"flash", "get_flash_status", "abort_flash", "get_flash_settings", "set_flash_settings"}},
			{"instance", {"get_instance_info"}},
		};

		const std::vector<std::string> registered = names();
		std::vector<std::string> expected;

		for (const auto& domain : domains)
		{
			for (const std::string& tool : domain.second)
			{
				expected.push_back(tool);

				if (std::find(registered.begin(), registered.end(), tool) == registered.end())
					ADD_FAILURE() << "tool is missing from the " << domain.first << " domain: " << tool;
			}
		}

		EXPECT_EQ(registered.size(), expected.size());
		EXPECT_EQ(registered.size(), 46u);
	}

	TEST_F(ServerTest, EveryToolDescribesItsArguments)
	{
		for (const mcp::Tool& tool : server->getRegistry().getTools())
		{
			EXPECT_FALSE(tool.description.empty()) << tool.name;
			EXPECT_EQ(tool.inputSchema.value("type", std::string()), "object") << tool.name;
			EXPECT_TRUE(tool.inputSchema.contains("properties")) << tool.name;
			EXPECT_TRUE(tool.inputSchema.at("properties").is_object()) << tool.name;

			/* A required name has to be one of the declared ones. */
			for (const auto& entry : tool.inputSchema.value("required", Json::array()))
			{
				const std::string field = entry.get<std::string>();
				EXPECT_TRUE(tool.inputSchema.at("properties").contains(field)) << tool.name << " requires undeclared " << field;
			}
		}
	}

	TEST_F(ServerTest, AnUnknownToolIsARequestTheClientGotWrong)
	{
		try
		{
			server->callTool("get_active_group", Json::object({}));
			ADD_FAILURE() << "the call should have been refused";
		}
		catch (const mcp::JsonRpcError& error)
		{
			EXPECT_EQ(error.getCode(), mcp::JsonRpcError::invalidParams);
			EXPECT_NE(std::string(error.what()).find("tools/list"), std::string::npos) << error.what();
		}
	}

	TEST_F(ServerTest, AMisspelledArgumentIsRefusedRatherThanIgnored)
	{
		/* Silently dropping it would let the caller believe it had configured
		   something, so the call has to fail and say what is accepted. */
		try
		{
			server->callTool("set_sampling", Json::object({{"sampl_frequency_hz", 100}}));
			ADD_FAILURE() << "the call should have been refused";
		}
		catch (const mcp::JsonRpcError& error)
		{
			EXPECT_EQ(error.getCode(), mcp::JsonRpcError::invalidParams);

			const std::string message = error.what();
			EXPECT_NE(message.find("'sampl_frequency_hz'"), std::string::npos) << message;
			EXPECT_NE(message.find("sample_frequency_hz"), std::string::npos) << message;
		}
	}

	TEST_F(ServerTest, SeveralUnknownArgumentsAreAllReported)
	{
		try
		{
			server->callTool("set_sampling", Json::object({{"freq", 100}, {"pornt", 10}}));
			ADD_FAILURE() << "the call should have been refused";
		}
		catch (const mcp::JsonRpcError& error)
		{
			const std::string message = error.what();
			EXPECT_NE(message.find("'freq'"), std::string::npos) << message;
			EXPECT_NE(message.find("'pornt'"), std::string::npos) << message;
		}
	}

	TEST_F(ServerTest, AToolWithoutArgumentsRefusesAny)
	{
		try
		{
			server->callTool("get_instance_info", Json::object({{"verbose", true}}));
			ADD_FAILURE() << "the call should have been refused";
		}
		catch (const mcp::JsonRpcError& error)
		{
			EXPECT_EQ(error.getCode(), mcp::JsonRpcError::invalidParams);
			EXPECT_NE(std::string(error.what()).find("takes no arguments"), std::string::npos) << error.what();
		}
	}

	TEST_F(ServerTest, DeclaredArgumentsReachTheToolBody)
	{
		/* No application is attached, so set_sampling stops at its first line and
		   reports the missing subsystem. That the call gets that far is the
		   point: a name the schema declares must not be treated as a typo. */
		const Json answer = callFromTheInterfaceThread("set_sampling", Json::object({{"sample_frequency_hz", 100}}));
		ASSERT_TRUE(answer.contains("isError")) << answer;
		EXPECT_TRUE(answer.at("isError").get<bool>());

		const std::string message = textOf(answer);
		EXPECT_NE(message.find("set_sampling"), std::string::npos) << message;
		EXPECT_EQ(message.find("Unknown argument"), std::string::npos) << message;
	}

	TEST_F(ServerTest, AModelToolIsCarriedOverToTheInterfaceThread)
	{
		/* The bridge is what keeps the model single threaded. No model is attached
		   here, so the tool reports the missing part, but the point is that the
		   call was queued, run by the interface thread and answered rather than
		   dropped. */
		const Json answer = callFromTheInterfaceThread("list_groups", Json::object({}));
		ASSERT_TRUE(answer.contains("isError")) << answer;

		const std::string message = textOf(answer);
		EXPECT_NE(message.find("list_groups"), std::string::npos) << message;
		EXPECT_EQ(message.find("did not process"), std::string::npos) << message;
		EXPECT_EQ(bridge.getCancelledCount(), 0u);
	}

	TEST_F(ServerTest, ACallTheApplicationNeverTakesIsAFailureOfTheTool)
	{
		/* Nobody drains the queue, so the call is dropped. It has to come back as
		   a failure of the tool: nothing about the request was wrong, the
		   application simply did not get to it. The short timeout keeps the test
		   quick. */
		mcp::Bridge untouched;
		mcp::Server unattended(&context, &untouched, logger.get());

		Json answer;
		std::thread caller([&answer, &unattended]()
						   { answer = unattended.callTool("list_groups", Json::object({})); });

		/* The queue is emptied without anything being run. */
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		untouched.cancelWaitingTasks();
		caller.join();

		ASSERT_TRUE(answer.contains("isError")) << answer;
		EXPECT_TRUE(answer.at("isError").get<bool>());
		EXPECT_FALSE(answer.contains("error")) << answer;
	}

	TEST_F(ServerTest, RequestsGoThroughTheRealServer)
	{
		const Json initialize = server->handleMessage(Json::object({{"jsonrpc", "2.0"},																	{"id", 1},
																	{"method", "initialize"},
																	{"params", {{"protocolVersion", "2025-06-18"}, {"capabilities", Json::object({})}}}}));
		ASSERT_TRUE(initialize.contains("result"));
		EXPECT_EQ(initialize.at("result").at("serverInfo").at("name"), "Variable-Trace");

		/* No application is attached here, so the version cannot be known. */
		EXPECT_EQ(initialize.at("result").at("serverInfo").at("version"), "unknown");

		const Json listing = server->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}));
		ASSERT_TRUE(listing.contains("result"));
		EXPECT_EQ(listing.at("result").at("tools").size(), 46u);

		const Json ping = server->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "ping"}}));
		EXPECT_EQ(ping.at("result"), Json::object({}));

		/* A notification stays unanswered. */
		EXPECT_TRUE(server->handleMessage(Json::object({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}})).is_null());

		/* A misspelled argument comes back as a protocol error through the same
		   path a real client would take. */
		const Json refused = server->handleMessage(Json::object({{"jsonrpc", "2.0"},
																 {"id", 4},
																 {"method", "tools/call"},
																 {"params", {{"name", "get_curve_data"}, {"arguments", {{"variabels", Json::array({"a"})}}}}}}));
		ASSERT_TRUE(refused.contains("error"));
		EXPECT_EQ(refused.at("error").at("code"), mcp::JsonRpcError::invalidParams);
	}

	/*
	 * The checks below bind real sockets on the loopback interface. Nothing is
	 * attached to the context, but the port search and the lifecycle are the
	 * ones a client meets.
	 */
	TEST_F(ServerTest, ATakenPortIsSteppedOver)
	{
		/* A second instance holds a port down. A single httplib server object
		   retires itself as soon as a bind fails, so this also pins down that a
		   fresh object is used per attempt rather than one object being retried
		   on the next port. */
		McpContext otherContext;
		mcp::Bridge otherBridge;
		mcp::Server other(&otherContext, &otherBridge, logger.get());

		ASSERT_TRUE(other.start(true, 0)) << other.getLastError();
		const uint16_t taken = other.getPort();
		ASSERT_GT(taken, 0);

		/* The request names the port that is already in use. */
		ASSERT_TRUE(server->start(true, taken)) << server->getLastError();
		EXPECT_TRUE(server->isRunning());
		EXPECT_GT(server->getPort(), taken);
		EXPECT_EQ(server->getEndpoint(), "http://127.0.0.1:" + std::to_string(server->getPort()) + "/mcp");

		server->stop();
		other.stop();
	}

	TEST_F(ServerTest, AServerWithoutAnApplicationContextIsNotStarted)
	{
		/* An application context is required, so the server refuses to come up
		   and says why instead of listening on something unexpected. */
		mcp::Server detached(nullptr, &bridge, logger.get());
		EXPECT_FALSE(detached.start(true, 0));
		EXPECT_FALSE(detached.isRunning());
		EXPECT_EQ(detached.getPort(), 0);
		EXPECT_FALSE(detached.getLastError().empty());
		EXPECT_EQ(detached.getEndpoint(), "");
	}

	TEST_F(ServerTest, ADisabledServerDoesNotListen)
	{
		EXPECT_FALSE(server->start(false, 0));
		EXPECT_FALSE(server->isRunning());
		EXPECT_EQ(server->getPort(), 0);

		/* The same object can be started afterwards, which is what the settings
		   window does when the switch is turned back on. */
		EXPECT_TRUE(server->start(true, 0)) << server->getLastError();
		EXPECT_TRUE(server->isRunning());
		EXPECT_GT(server->getPort(), 0);

		server->stop();
		EXPECT_FALSE(server->isRunning());
		EXPECT_EQ(server->getPort(), 0);
		EXPECT_EQ(server->getEndpoint(), "");
	}

	TEST_F(ServerTest, RestartingMovesTheListenerToTheRequestedPort)
	{
		McpContext otherContext;
		mcp::Bridge otherBridge;
		mcp::Server other(&otherContext, &otherBridge, logger.get());
		ASSERT_TRUE(other.start(true, 0)) << other.getLastError();
		const uint16_t taken = other.getPort();

		ASSERT_TRUE(server->start(true, 0)) << server->getLastError();
		const uint16_t first = server->getPort();
		ASSERT_GT(first, 0);

		/* Loading a project asks for a restart with the settings of the project
		   rather than of the previous run. The requested port is the one the
		   other instance holds, so the restart has to step over it again. */
		server->restart(true, taken);
		EXPECT_TRUE(server->isRunning());
		EXPECT_GT(server->getPort(), taken);

		server->restart(false, 0);
		EXPECT_FALSE(server->isRunning());
		EXPECT_EQ(server->getPort(), 0);

		other.stop();
	}
}  // namespace
