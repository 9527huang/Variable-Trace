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

/******************************************************************************
 * Changelog
 * -----------------------------------------------------------------------------
 * Date        | Version | Author           | Description
 * ------------|---------|------------------|-----------------------------------
 * 2025-10-05  | 1.0     | P. Wasilewski    | Initial version
 * ------------|---------|------------------|-----------------------------------
 * 2026-03-01  | 1.1     | P. Wasilewski    | C2000 address fix 
 * ------------|---------|------------------|-----------------------------------
 * 2026-03-11  | 1.2     | P. Wasilewski    | Fixed C2000 macro in Defines file
 *****************************************************************************/

#ifndef __SERIALDRIVER_H
#define __SERIALDRIVER_H

#include <stdint.h>

#include "serialDriverDefines.h"

/* -------- PLEASE IMPLEMENT THE FUNCTION BELOW IN YOUR CODE -------- */

#if ____SERIAL_DRIVER_C2000_SUPPORT == 1
extern void serialDriverSendData(uint16_t* buf, uint16_t size);
#else
extern void serialDriverSendData(uint8_t* buf, uint16_t size);
#endif

/* ----------------------------------------------------------------- */

#define ____SERIAL_DRIVER_VERSION  (1)
#define ____SERIAL_DRIVER_REVISION (2)

#define ____SERIAL_DRIVER_SOF1				 (0xAA)
#define ____SERIAL_DRIVER_SOF2				 (0x55)
#define ____SERIAL_DRIVER_RESPONSE_OK		 (0x00)
#define ____SERIAL_DRIVER_ADDRESS_LEN		 (4)
#define ____SERIAL_DRIVER_CRC_LEN			 (2)
#define ____SERIAL_DRIVER_MAX_VAR_LEN		 (4)
#define ____SERIAL_DRIVER_MAX_TX_BUFFER_SIZE ((____SERIAL_DRIVER_MAX_VAR_LEN * ____SERIAL_DRIVER_MAXVARS) + ____SERIAL_DRIVER_CRC_LEN)

typedef enum
{
	____SERIAL_DRIVER_STATE_WAIT_SOF1,
	____SERIAL_DRIVER_STATE_WAIT_SOF2,
	____SERIAL_DRIVER_STATE_COMMAND,
	____SERIAL_DRIVER_STATE_LENGTH,
	____SERIAL_DRIVER_STATE_ADDRESS,
	____SERIAL_DRIVER_STATE_SIZE,
	____SERIAL_DRIVER_STATE_DATA,
	____SERIAL_DRIVER_STATE_CRC1,
	____SERIAL_DRIVER_STATE_CRC2
} ____SerialDriverState;

#if ____SERIAL_DRIVER_C2000_SUPPORT == 1
typedef uint16_t ____SerialDriverDataType;
typedef uint32_t ____SerialDriverAddressType;
typedef uint16_t ____SerialDriverStateType;
#else
typedef uint8_t ____SerialDriverDataType;
typedef uint32_t ____SerialDriverAddressType;
typedef ____SerialDriverState ____SerialDriverStateType;
#endif

typedef enum
{
	SERIAL_CMD_READ = 0x01,
	SERIAL_CMD_WRITE = 0x02,
	SERIAL_CMD_BUFFER_READ = 0x03
} ____SerialDriverCommand;

typedef struct
{
	____SerialDriverAddressType addr;
	uint16_t size;
} ____SerialDriverBulkEntry;

typedef struct
{
	____SerialDriverStateType state;
	uint16_t rxCmd;
	uint16_t rxLen;
	uint32_t rxAddr;
	uint32_t rxData;
	____SerialDriverDataType txData[____SERIAL_DRIVER_MAX_TX_BUFFER_SIZE];
	____SerialDriverBulkEntry rxBulk[____SERIAL_DRIVER_MAXVARS];
	uint16_t rxBulkCount;
	uint16_t bulkIndex;
	uint16_t rxIndex;
	uint32_t bufReadAddr;
	uint16_t bufReadSize;
	uint16_t rxCrc;
	uint16_t rxCalculatedCrc;
	____SerialDriverDataType response;
} ____SerialDriver;

typedef struct
{
	uint32_t version;
	uint32_t revision;
	uint32_t maxVariables;
} ____SerialDriverSettings;

static inline void ____serialDriverMemcpy(____SerialDriverDataType* dst, ____SerialDriverDataType* src, uint32_t size)
{
#if ____SERIAL_DRIVER_C2000_SUPPORT == 1
	size = size >> 1;
#endif

	uint32_t i = 0;
	for (i = 0; i < size; i++)
		*dst++ = *src++;
}

static inline void ____serialDriverMemcpyWordToByte(____SerialDriverDataType* dst, ____SerialDriverDataType* src, uint32_t size)
{
#if ____SERIAL_DRIVER_C2000_SUPPORT == 1
	uint32_t i = 0;
	for (i = 0; i < (size >> 1); i++)
	{
		*dst++ = ((*src) & 0xff);
		*dst++ = (((*src) >> 8) & 0xff);
		src++;
	}
#else
	____serialDriverMemcpy(dst, src, size);
#endif
}

static inline void ____serialDriverCRC16Update(uint16_t* crc, ____SerialDriverDataType byte)
{
	extern const uint16_t ____crc16NibbleTable[16];

	*crc = (*crc >> 4) ^ ____crc16NibbleTable[(*crc & 0x0F) ^ (byte & 0x0F)];
	*crc = (*crc >> 4) ^ ____crc16NibbleTable[(*crc & 0x0F) ^ (byte >> 4)];
}

static inline uint16_t ____serialDriverCRC16(const ____SerialDriverDataType* data, uint16_t len)
{
	uint16_t crc = 0xFFFF;
	uint16_t i = 0;
	for (i = 0; i < len; i++)
	{
		____serialDriverCRC16Update(&crc, data[i]);
	}
	return crc;
}

static inline void ____serialDriverHandleFrame(void)
{
	extern ____SerialDriver ____serialDriver;

	if (____serialDriver.rxCmd == SERIAL_CMD_WRITE)
	{
		____serialDriverMemcpy((____SerialDriverDataType*)____serialDriver.rxAddr, (____SerialDriverDataType*)&____serialDriver.rxData, ____serialDriver.rxLen);
		____serialDriver.response = ____SERIAL_DRIVER_RESPONSE_OK;
		serialDriverSendData(&____serialDriver.response, 1);
	}
	else if (____serialDriver.rxCmd == SERIAL_CMD_BUFFER_READ)
	{
		serialDriverSendData((____SerialDriverDataType*)____serialDriver.bufReadAddr, ____serialDriver.bufReadSize);
	}
	else if (____serialDriver.rxCmd == SERIAL_CMD_READ)
	{
		uint16_t j = 0;
		uint16_t i = 0;
		for (i = 0; i < ____serialDriver.rxBulkCount; i++)
		{
			____serialDriverMemcpyWordToByte(&____serialDriver.txData[j], (____SerialDriverDataType*)____serialDriver.rxBulk[i].addr, ____serialDriver.rxBulk[i].size);
			j += ____serialDriver.rxBulk[i].size;
		}

		uint16_t crc = ____serialDriverCRC16(____serialDriver.txData, j);
		____serialDriverMemcpyWordToByte(&____serialDriver.txData[j], (____SerialDriverDataType*)&crc, ____SERIAL_DRIVER_CRC_LEN);
		serialDriverSendData((____SerialDriverDataType*)____serialDriver.txData, j + ____SERIAL_DRIVER_CRC_LEN);
	}
}

static inline void serialDriverReceiveByte(____SerialDriverDataType byte)
{
	extern ____SerialDriver ____serialDriver;
#if ____SERIAL_DRIVER_C2000_SUPPORT == 1
	extern ____SerialDriverSettings ____serialDriverSettings;
#else
	extern volatile ____SerialDriverSettings ____serialDriverSettings;
#endif

	(void)____serialDriverSettings.version;

	switch (____serialDriver.state)
	{
		case ____SERIAL_DRIVER_STATE_WAIT_SOF1:
			if (byte == ____SERIAL_DRIVER_SOF1)
			{
				____serialDriver.rxCalculatedCrc = 0xFFFF;
				____serialDriverCRC16Update(&____serialDriver.rxCalculatedCrc, byte);
				____serialDriver.state = ____SERIAL_DRIVER_STATE_WAIT_SOF2;
			}
			break;

		case ____SERIAL_DRIVER_STATE_WAIT_SOF2:
			if (byte == ____SERIAL_DRIVER_SOF2)
			{
				____serialDriverCRC16Update(&____serialDriver.rxCalculatedCrc, byte);
				____serialDriver.state = ____SERIAL_DRIVER_STATE_COMMAND;
			}
			else
				____serialDriver.state = ____SERIAL_DRIVER_STATE_WAIT_SOF1;
			break;

		case ____SERIAL_DRIVER_STATE_COMMAND:
			____serialDriver.rxCmd = byte;
			if (____serialDriver.rxCmd != SERIAL_CMD_WRITE && ____serialDriver.rxCmd != SERIAL_CMD_BUFFER_READ && ____serialDriver.rxCmd != SERIAL_CMD_READ)
				____serialDriver.state = ____SERIAL_DRIVER_STATE_WAIT_SOF1;
			else
			{
				____serialDriverCRC16Update(&____serialDriver.rxCalculatedCrc, byte);
				____serialDriver.state = ____SERIAL_DRIVER_STATE_LENGTH;
			}
			break;

		case ____SERIAL_DRIVER_STATE_LENGTH:
			if (____serialDriver.rxCmd == SERIAL_CMD_READ)
			{
				____serialDriver.rxBulkCount = byte;
				if (____serialDriver.rxBulkCount == 0 || ____serialDriver.rxBulkCount > ____SERIAL_DRIVER_MAXVARS)
					____serialDriver.state = ____SERIAL_DRIVER_STATE_WAIT_SOF1;
				else
				{
					____serialDriver.rxBulk[0].addr = 0;
					____serialDriver.bulkIndex = 0;
					____serialDriver.rxIndex = 0;
					____serialDriverCRC16Update(&____serialDriver.rxCalculatedCrc, byte);
					____serialDriver.state = ____SERIAL_DRIVER_STATE_ADDRESS;
				}
			}
			else if (____serialDriver.rxCmd == SERIAL_CMD_BUFFER_READ)
			{
				____serialDriverCRC16Update(&____serialDriver.rxCalculatedCrc, byte);
				____serialDriver.bufReadSize = byte;
				____serialDriver.rxIndex = 0;
				____serialDriver.bufReadAddr = 0;
				____serialDriver.state = ____SERIAL_DRIVER_STATE_ADDRESS;
			}
			else if (____serialDriver.rxCmd == SERIAL_CMD_WRITE)
			{
				____serialDriver.rxLen = byte;
				____serialDriverCRC16Update(&____serialDriver.rxCalculatedCrc, byte);
				____serialDriver.rxIndex = 0;
				____serialDriver.rxAddr = 0;
				____serialDriver.state = ____SERIAL_DRIVER_STATE_ADDRESS;
			}
			break;

		case ____SERIAL_DRIVER_STATE_ADDRESS:

			____serialDriverCRC16Update(&____serialDriver.rxCalculatedCrc, byte);

			if (____serialDriver.rxCmd == SERIAL_CMD_READ)
			{
				____serialDriver.rxBulk[____serialDriver.bulkIndex].addr |= ((uint32_t)byte << (8 * ____serialDriver.rxIndex++));
				if (____serialDriver.rxIndex >= ____SERIAL_DRIVER_ADDRESS_LEN)
				{
					____serialDriver.rxIndex = 0;
					____serialDriver.state = ____SERIAL_DRIVER_STATE_SIZE;
				}
			}
			else if (____serialDriver.rxCmd == SERIAL_CMD_BUFFER_READ)
			{
				____serialDriver.bufReadAddr |= ((uint32_t)byte << (8 * ____serialDriver.rxIndex++));
				if (____serialDriver.rxIndex >= ____SERIAL_DRIVER_ADDRESS_LEN)
					____serialDriver.state = ____SERIAL_DRIVER_STATE_CRC1;
			}
			else if (____serialDriver.rxCmd == SERIAL_CMD_WRITE)
			{
				____serialDriver.rxAddr |= ((uint32_t)byte << (8 * ____serialDriver.rxIndex++));
				if (____serialDriver.rxIndex >= ____SERIAL_DRIVER_ADDRESS_LEN)
				{
					____serialDriver.rxIndex = 0;
					____serialDriver.rxData = 0;
					____serialDriver.state = ____SERIAL_DRIVER_STATE_DATA;
				}
			}
			break;

		case ____SERIAL_DRIVER_STATE_SIZE:
			____serialDriverCRC16Update(&____serialDriver.rxCalculatedCrc, byte);
			____serialDriver.rxBulk[____serialDriver.bulkIndex].size = byte;
			____serialDriver.bulkIndex++;
			if (____serialDriver.bulkIndex >= ____serialDriver.rxBulkCount)
				____serialDriver.state = ____SERIAL_DRIVER_STATE_CRC1;
			else
			{
				____serialDriver.rxIndex = 0;
				____serialDriver.state = ____SERIAL_DRIVER_STATE_ADDRESS;
				____serialDriver.rxBulk[____serialDriver.bulkIndex].addr = 0;
			}
			break;

		case ____SERIAL_DRIVER_STATE_DATA:
			____serialDriverCRC16Update(&____serialDriver.rxCalculatedCrc, byte);
			____serialDriver.rxData |= ((uint32_t)byte << (8 * ____serialDriver.rxIndex));
			____serialDriver.rxIndex++;
			if (____serialDriver.rxIndex >= ____serialDriver.rxLen)
				____serialDriver.state = ____SERIAL_DRIVER_STATE_CRC1;
			break;

		case ____SERIAL_DRIVER_STATE_CRC1:
			____serialDriver.rxCrc = byte;
			____serialDriver.state = ____SERIAL_DRIVER_STATE_CRC2;
			break;

		case ____SERIAL_DRIVER_STATE_CRC2:
			____serialDriver.rxCrc |= ((uint16_t)byte << 8);

			if (____serialDriver.rxCalculatedCrc == ____serialDriver.rxCrc)
				____serialDriverHandleFrame();

			____serialDriver.state = ____SERIAL_DRIVER_STATE_WAIT_SOF1;
			break;
	}
}

#endif /* SERIALDRIVER_SERIALDRIVER_H_ */
