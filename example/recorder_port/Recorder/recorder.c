/******************************************************************************
 *                                                                            *
 *  MCUViewer Project                                                         *
 *                                                                            *
 *  (c) 2025 Piotr Wasilewski                                                 *
 *  https://mcuviewer.com                                                     *
 *                                                                            *
 *  All rights reserved.                                                      *
 *                                                                            *
 *  This file is licensed for use exclusively with the MCUViewer software.    *
 *  Permission is granted to use, modify, and distribute this file,           *
 *  in binary or source form, **only as part of or in connection with**       *
 *  the MCUViewer project, in private or commercial settings, provided        *
 *  this copyright notice and disclaimer are preserved without changes.       *
 *                                                                            *
 *  THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND,           *
 *  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO WARRANTIES OF            *
 *  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.   *
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR          *
 *  OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,     *
 *  ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR     *
 *  OTHER DEALINGS IN THE SOFTWARE.                                           *
 *                                                                            *
 ******************************************************************************/

#include "recorder.h"

____Recorder ____recorder = {0};

#if ____RECORDER_C2000_SUPPORT == 1
____RecorderSettings ____recorderSettings=
#else
volatile ____RecorderSettings ____recorderSettings=
#endif
	{
		.version = ____RECORDER_VERSION,
		.revision = ____RECORDER_REVISION,
		.timestepNs = ____RECORDER_TIMEBASE_NS,
		.maxBufferSize = ____RECORDER_BUFFERSIZE,
		.maxVariables = ____RECORDER_MAXVARS,
		.floatSupport = ____RECORDER_FLOAT_SUPPORT,
};
