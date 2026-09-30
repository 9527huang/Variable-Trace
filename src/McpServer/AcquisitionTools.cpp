#include <cstdint>
#include <optional>
#include <string>

#include "McpTools.hpp"

namespace mcp
{
	namespace
	{
		/* Index of the probe in IDebugProbe::DebugProbeSettings::debugProbe. The
		   order matches the probe list of the acquisition settings window. */
		constexpr uint32_t stlinkProbe = IDebugProbe::Probe::Stlink;
		constexpr uint32_t jlinkProbe = IDebugProbe::Probe::Jlink;
		constexpr uint32_t serialProbe = IDebugProbe::Probe::Serial;

		/* The highest baud rate a serial port can be driven at is well below
		   this, and the field has to stay inside the processor's range. */
		constexpr int64_t maxBaudrate = 12000000;

		std::string probeTypeToString(uint32_t type)
		{
			switch (type)
			{
				case jlinkProbe:
					return "jlink";

				case serialProbe:
					return "serial";

				default:
					return "stlink";
			}
		}

		uint32_t probeTypeFromString(const std::string& name)
		{
			if (name == "stlink")
				return stlinkProbe;

			if (name == "jlink")
				return jlinkProbe;

			if (name == "serial")
				return serialProbe;

			if (name == "gdbserver" || name == "stlink_cubeprog")
				reportUnavailable("the '" + name + "' debug probe", "set_debug_probe");

			throw ToolError("Unknown probe type '" + name + "'. Use 'stlink', 'jlink' or 'serial'.");
		}

		/* Probe settings are read by the acquisition thread when it starts the
		   probe, so changing them during a run would either have no effect or
		   race with the running read loop. */
		void requireAcquisitionStopped(McpContext& context, const std::string& what)
		{
			if (context.viewerDataHandler == nullptr)
				return;

			if (context.viewerDataHandler->getStateImmediate() != DataHandlerBase::State::RUN)
				return;

			throw ToolError("Acquisition is running, so " + what + " would not take effect. Call stop_acquisition first.");
		}

		Json setDebugProbe(McpContext& context, const Json& arguments)
		{
			if (context.viewerDataHandler == nullptr)
				reportUnavailable("debug probe settings", "set_debug_probe");

			requireAcquisitionStopped(context, "a change of the debug probe settings");

			IDebugProbe::DebugProbeSettings settings = context.viewerDataHandler->getProbeSettings();

			const bool typeProvided = has(arguments, "type");
			const uint32_t type = typeProvided ? probeTypeFromString(requireString(arguments, "type")) : settings.debugProbe;

			/* The interface of the J-Link is opened over USB and closed when a run
			   ends, so a request to hold it open cannot be honoured. */
			if (has(arguments, "keep_connection") && optionalBool(arguments, "keep_connection").value_or(false))
				throw ToolError("This build closes the probe connection after every run, so 'keep_connection' cannot be enabled. Omit the field.");

			if (has(arguments, "mode"))
			{
				const std::string mode = optionalString(arguments, "mode").value_or("");

				if (mode == "normal")
					settings.mode = IDebugProbe::Mode::NORMAL;
				else if (mode == "hss")
				{
					if (type != jlinkProbe)
						throw ToolError("HSS mode is available for the J-Link probe only.");

					settings.mode = IDebugProbe::Mode::HSS;
				}
				else
					throw ToolError("Unknown probe mode '" + mode + "'. Use 'normal' or 'hss'.");
			}
			else if (type != jlinkProbe)
				settings.mode = IDebugProbe::Mode::NORMAL;

			if (has(arguments, "serial_number"))
				settings.serialNumber = optionalString(arguments, "serial_number").value_or("");

			if (has(arguments, "speed_khz"))
			{
				const int64_t speed = *optionalInteger(arguments, "speed_khz");

				if (speed <= 0 || speed > 100000)
					throw ToolError("Field 'speed_khz' must be between 1 and 100000.");

				settings.speedkHz = static_cast<uint32_t>(speed);
			}

			settings.debugProbe = type;

			context.viewerDataHandler->setProbeSettings(settings);

			/* The object the acquisition uses follows the configured type, which is
			   what the application does after the same change. */
			if (typeProvided && context.selectProbeDevice)
				context.selectProbeDevice();

			Json result = Json::object({{"type", probeTypeToString(settings.debugProbe)},
										{"serial_number", settings.serialNumber},
										{"speed_khz", settings.speedkHz},
										{"mode", settings.mode == IDebugProbe::Mode::HSS ? "hss" : "normal"},
										{"keep_connection", false}});

			/* The port name travels in the field the two hardware probes use for
			   the serial number, so the reply labels it for what it is. */
			if (settings.debugProbe == serialProbe)
				result["baudrate"] = settings.baudrate;

			return result;
		}

		Json configureProbe(McpContext& context, const Json& arguments)
		{
			if (context.viewerDataHandler == nullptr)
				reportUnavailable("debug probe settings", "configure_probe");

			const IDebugProbe::DebugProbeSettings settings = context.viewerDataHandler->getProbeSettings();
			const uint32_t type = settings.debugProbe;

			/* The fields of probes this build does not have are reported before
			   anything is written, so a call never applies half of its arguments. */
			if (has(arguments, "ip") || has(arguments, "port"))
				reportUnavailable("the GDB server probe", "configure_probe");

			if (has(arguments, "api_path"))
				reportUnavailable("the ST-Link CubeProgrammer probe", "configure_probe");

			if (has(arguments, "script_enabled") || has(arguments, "script_path"))
				reportUnavailable("J-Link script files", "configure_probe");

			const bool deviceProvided = has(arguments, "device");
			const bool connectionProvided = has(arguments, "connection");
			const bool baudrateProvided = has(arguments, "baudrate");

			/* Every probe accepts the settings that belong to it and nothing
			   else, so a field written for the wrong probe is refused instead of
			   being stored where it would never be read. */
			if (type == serialProbe)
			{
				if (deviceProvided || connectionProvided)
					throw ToolError("'device' and 'connection' apply to the J-Link probe, and the active probe is 'serial'. Set the port with set_debug_probe and the line speed with 'baudrate'.");

				if (!baudrateProvided)
					throw ToolError("No probe setting was provided. Pass 'baudrate'.");
			}
			else if (type == jlinkProbe)
			{
				if (baudrateProvided)
					throw ToolError("'baudrate' applies to the serial probe, and the active probe is 'jlink'.");

				if (!deviceProvided && !connectionProvided)
					throw ToolError("No probe setting was provided. Pass 'device' and/or 'connection'.");
			}
			else
			{
				if (deviceProvided || connectionProvided || baudrateProvided)
					throw ToolError("The active probe '" + probeTypeToString(type) + "' has no type specific settings.");
			}

			requireAcquisitionStopped(context, "a change of the probe settings");

			if (baudrateProvided)
			{
				const std::optional<int64_t> baudrate = optionalInteger(arguments, "baudrate");

				if (!baudrate.has_value() || *baudrate <= 0 || *baudrate > maxBaudrate)
					throw ToolError("Field 'baudrate' must be between 1 and " + std::to_string(maxBaudrate) + ".");

				IDebugProbe::DebugProbeSettings updated = settings;
				updated.baudrate = static_cast<uint32_t>(*baudrate);
				updated.mode = IDebugProbe::Mode::NORMAL;
				context.viewerDataHandler->setProbeSettings(updated);

				return Json::object({{"probe", probeTypeToString(updated.debugProbe)},
									 {"port", updated.serialNumber},
									 {"baudrate", updated.baudrate}});
			}

			IDebugProbe::DebugProbeSettings updated = settings;

			if (deviceProvided)
			{
				const std::string device = optionalString(arguments, "device").value_or("");

				if (device.empty())
					throw ToolError("Field 'device' must not be empty. Pass the J-Link device name, for example 'STM32F103C8'.");

				/* A trailing space makes the J-Link refuse the name with 'Failed to
				   set device', which is hard to notice in a form field. */
				if (device.find_last_not_of(" \t") != device.size() - 1)
					throw ToolError("Field 'device' must not end with whitespace, the J-Link rejects such a name with 'Failed to set device'.");

				updated.device = device;
			}

			if (connectionProvided)
			{
				const std::string connection = optionalString(arguments, "connection").value_or("");

				if (connection == "tcp_ip")
					reportUnavailable("the J-Link TCP/IP connection", "configure_probe");

				if (connection != "usb")
					throw ToolError("Unknown J-Link connection '" + connection + "'. Use 'usb'.");
			}

			context.viewerDataHandler->setProbeSettings(updated);

			return Json::object({{"probe", probeTypeToString(updated.debugProbe)},
								 {"device", updated.device},
								 {"connection", "usb"}});
		}

		Json startAcquisition(McpContext& context, const Json&)
		{
			if (context.viewerDataHandler == nullptr)
				reportUnavailable("the viewer data handler", "start_acquisition");

			const bool wasRunning = context.viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN;

			if (!wasRunning)
				context.viewerDataHandler->setState(DataHandlerBase::State::RUN);

			/* The probe is opened by the acquisition thread after this returns, so
			   a failure shows up in get_acquisition_state as a state of STOP with
			   'last_error' set rather than in this reply. */
			return Json::object({{"state", "RUN"}, {"changed", !wasRunning}});
		}

		Json stopAcquisition(McpContext& context, const Json&)
		{
			if (context.viewerDataHandler == nullptr)
				reportUnavailable("the viewer data handler", "stop_acquisition");

			const bool wasRunning = context.viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN;

			if (wasRunning)
				context.viewerDataHandler->setState(DataHandlerBase::State::STOP);

			return Json::object({{"state", "STOP"}, {"changed", wasRunning}});
		}

		Json getAcquisitionState(McpContext& context, const Json&)
		{
			if (context.viewerDataHandler == nullptr)
				reportUnavailable("the viewer data handler", "get_acquisition_state");

			const bool running = context.viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN;
			Json result = Json::object({{"state", running ? "RUN" : "STOP"}});

			/* The probe keeps the reason a start failed, and it is the only place
			   that reason survives, so it is reported while stopped. */
			if (!running)
			{
				const std::string error = context.viewerDataHandler->getLastReaderError();

				if (!error.empty())
					result["last_error"] = error;
			}

			return result;
		}
	}  // namespace

	void AcquisitionTools::registerAll(ToolRegistry& registry)
	{
		registry.add({.name = "set_debug_probe",
					  .description = "Set the debug probe type and common connection settings. Use configure_probe for type-specific settings. "
									 "  type          : 'stlink'|'jlink'|'gdbserver'|'serial'|'stlink_cubeprog' "
									 "  serial_number : probe serial or device path "
									 "  speed_khz     : SWD/JTAG speed (ignored for serial/gdbserver) "
									 "  mode          : 'normal' or 'hss' "
									 "  keep_connection: 1 = keep connected between runs (default 1)",
					  .inputSchema = objectSchema(Json::object({{"type", enumProperty("'stlink', 'jlink', 'gdbserver', 'serial', or 'stlink_cubeprog'", {"stlink", "jlink", "gdbserver", "serial", "stlink_cubeprog"})},
																{"serial_number", stringProperty("Probe serial number or device identifier")},
																{"speed_khz", integerProperty("SWD/JTAG speed in kHz")},
																{"mode", enumProperty("'normal' or 'hss'", {"normal", "hss"})},
																{"keep_connection", booleanProperty("1 = keep connection alive between runs (default 1)")}})),
					  .handler = setDebugProbe});

		registry.add({.name = "configure_probe",
					  .description = "Configure probe-type-specific settings (call set_debug_probe first). "
									 "  J-Link   : device, connection ('usb'/'tcp_ip'), script_enabled (0/1), script_path. "
									 "  GDB server: ip, port. "
									 "  Serial   : baudrate. "
									 "  CubeProg : api_path (empty = auto-detect).",
					  .inputSchema = objectSchema(Json::object({{"device", stringProperty("J-Link target device (e.g. 'STM32F407VG')")},
																{"connection", enumProperty("J-Link connection: 'usb' or 'tcp_ip'", {"usb", "tcp_ip"})},
																{"script_enabled", booleanProperty("J-Link script: 1=on, 0=off")},
																{"script_path", stringProperty("J-Link script file path")},
																{"ip", stringProperty("GDB server IP address")},
																{"port", integerProperty("GDB server port")},
																{"baudrate", integerProperty("Serial baud rate")},
																{"api_path", stringProperty("CubeProgrammer API library path (empty = auto-detect)")}})),
					  .handler = configureProbe});

		registry.add({.name = "start_acquisition",
					  .description = "Start MCUViewer data acquisition. Transitions the viewer from STOP to RUN state so that variable values are read from the target device.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = startAcquisition});

		registry.add({.name = "stop_acquisition",
					  .description = "Stop MCUViewer data acquisition. Transitions the viewer from RUN to STOP state.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = stopAcquisition});

		registry.add({.name = "get_acquisition_state",
					  .description = "Query the current MCUViewer acquisition state. Returns {state}, where state is 'RUN' or 'STOP'.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = getAcquisitionState});
	}
}  // namespace mcp
