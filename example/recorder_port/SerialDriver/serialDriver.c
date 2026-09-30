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

#include "serialDriver.h"

#if ____SERIAL_DRIVER_C2000_SUPPORT == 1
____SerialDriverSettings ____serialDriverSettings=
#else
volatile ____SerialDriverSettings ____serialDriverSettings=
#endif
{
		.version = ____SERIAL_DRIVER_VERSION,
		.revision = ____SERIAL_DRIVER_REVISION,
		.maxVariables = ____SERIAL_DRIVER_MAXVARS,
};

____SerialDriver ____serialDriver = {0};

const uint16_t ____crc16NibbleTable[16] = {
	0x0000, 0xC0C1, 0xC181, 0x0140,
	0xC301, 0x03C0, 0x0280, 0xC241,
	0xC601, 0x06C0, 0x0780, 0xC741,
	0x0500, 0xC5C1, 0xC481, 0x0440};
