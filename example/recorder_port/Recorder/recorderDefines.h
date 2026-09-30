#ifndef __RECORDER_DEFINES_H_
#define __RECORDER_DEFINES_H_

/* -------- PLEASE FILL IN THE FIELDS BELOW -------- */

// TUTORIAL: https://docs.mcuviewer.com/recorder/
// EXAMPLE: https://docs.mcuviewer.com/gdbserver/#raspberry-pi-debug-probe-example-recorder

#define ____RECORDER_TIMEBASE_NS   (10000)	// Time period between two consecutive recorderStep() calls in nanoseconds.
#define ____RECORDER_BUFFERSIZE	   (32768)	// Size of recorder buffer - the more samples the longer recording can be done.
#define ____RECORDER_MAXVARS	   (12)		// Limit of recorder variables. Can be used as limiter if recorder overhead is too high for higher number of variables.
#define ____RECORDER_FLOAT_SUPPORT (1)		// Float support enable/disable. On targets that do not have FPU it is advised to disable it.

#define ____RECORDER_C2000_SUPPORT (0)      // C2000 support enable/disable

/* ------------------------------------------------- */

#endif
