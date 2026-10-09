#ifndef _ITRACEPROBE_HPP
#define _ITRACEPROBE_HPP

#include <stdint.h>

#include <string>
#include <vector>

class ITraceProbe
{
   public:
	/* How a J-Link is reached. Kept here rather than borrowed from the debug
	   probe: the two hierarchies are independent, and the value is only ever
	   written down as a number in a project file. The USB value is the one a
	   project without the setting was written with. */
	enum class Connection
	{
		Usb = 0,
		Network = 1,
	};

	typedef struct
	{
		uint32_t debugProbe = 0;
		std::string serialNumber = "";
		std::string device = "";
		uint32_t speedkHz = 10000;

		/* J-Link only, and the same setting as on the variable side: closing the
		   link when a run ends halts the target on some setups. */
		bool keepConnection = false;

		/* J-Link only. How the probe is reached, where it is when that is over
		   the network, and a script of the reader's own to run before the target
		   is connected. */
		Connection connection = Connection::Usb;
		std::string ipAddress = "";
		bool scriptEnabled = false;
		std::string scriptPath = "";

	} TraceProbeSettings;

	virtual ~ITraceProbe() = default;
	virtual bool startTrace(const TraceProbeSettings& probeSettings, uint32_t coreFrequency, uint32_t tracePrescaler, uint32_t activeChannelMask, bool shouldReset) = 0;
	virtual bool stopTrace() = 0;
	virtual int32_t readTraceBuffer(uint8_t* buffer, uint32_t size) = 0;

	virtual std::string getTargetName() = 0;
	virtual std::vector<std::string> getConnectedDevices() = 0;

	/* Whether the link to the target is open right now. A run that ends usually
	   closes it, but the reader can ask for it to outlive the run, and the
	   interface says which of the two it is looking at. */
	virtual bool isLinkOpen() const { return false; }

	/* reason why the last startTrace() failed, empty when the last attempt succeeded */
	virtual std::string getLastErrorMsg() const = 0;

   protected:
	std::string lastErrorMsg = "";
};

#endif