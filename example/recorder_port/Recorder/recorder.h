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
 * 2025-05-23  | 8.1     | P. Wasilewski    | Initial version
 * ------------|---------|------------------|-----------------------------------
 * 2025-06-15  | 8.2     | P. Wasilewski    | Separated recorder settings struct
 * ------------|---------|------------------|-----------------------------------
 * 2025-12-28  | 8.3     | P. Wasilewski    | Adopted for C2000 family
 *****************************************************************************/

#ifndef __RECORDER_H
#define __RECORDER_H

#include <stdint.h>

#include "recorderDefines.h"

#define ____RECORDER_VERSION  (8)
#define ____RECORDER_REVISION (3)


typedef enum
{
	____STATE_INIT = 0,
	____STATE_RUNNING,
	____STATE_FULL
} ____RecorderState;

typedef enum
{
	____UNKNOWN = 0,
	____U8 = 1,
	____I8 = 2,
	____U16 = 3,
	____I16 = 4,
	____U32 = 5,
	____I32 = 6,
	____F32 = 7
} ____TriggerType;

typedef enum
{
	____EDGE_RISING = 0,
	____EDGE_FALLING,
} ____TriggerEdge;

typedef enum
{
	____STATE_SETUP = 0,
	____STATE_READY,
	____STATE_TRIGGERED,
	____STATE_POSTTRIGGER
} ____TriggerState;

#if ____RECORDER_C2000_SUPPORT == 1
typedef uint16_t ____RecorderDataType;
typedef uint16_t ____RecorderStateType;
typedef uint16_t ____TriggerTypeType;
typedef uint16_t ____TriggerEdgeType;
typedef uint16_t ____TriggerStateType;
#else
typedef uint8_t ____RecorderDataType;
typedef ____RecorderState ____RecorderStateType;
typedef ____TriggerType ____TriggerTypeType;
typedef ____TriggerEdge ____TriggerEdgeType;
typedef ____TriggerState ____TriggerStateType;
#endif

typedef struct
{
	uint32_t head;
	uint32_t start;
	____RecorderDataType data[____RECORDER_BUFFERSIZE];
} ____Buffer;

typedef struct
{
	uint32_t address;
	uint32_t size;
} ____Variable;

typedef struct
{
	____TriggerStateType state;
	____TriggerEdgeType edge;
	____TriggerTypeType type;

	uint32_t sourceAddress;
	uint32_t preTriggerSamples;
	int32_t triggerValue;
	int32_t actValue;
	int32_t prevValue;
	uint32_t triggerValueU32;
	uint32_t actValueU32;
	uint32_t prevValueU32;
#if ____RECORDER_FLOAT_SUPPORT == 1
	float triggerValueFloat;
	float actValueFloat;
	float prevValueFloat;
#endif

} ____Trigger;

typedef struct
{
	uint32_t samplesToSkip;
	uint32_t variableCount;
	uint32_t maxActualBufferSize;
	uint16_t samplesPackSize;
	uint32_t samplesSinceStart;
	uint32_t skippedSamples;

	____RecorderStateType state;
	____Variable sampleList[____RECORDER_MAXVARS];
	____Buffer recorderbuf;
	____Trigger trigger;

} ____Recorder;

typedef struct
{
	uint32_t version;
	uint32_t revision;
	uint32_t timestepNs;
	uint32_t maxBufferSize;
	uint32_t maxVariables;
	uint16_t floatSupport;
} ____RecorderSettings;



#if ____RECORDER_C2000_SUPPORT == 1
static inline void ____recorderMemcpy(____RecorderDataType* src, ____RecorderDataType* dst, uint32_t size)
{
	uint32_t i = 0;
	for (i = 0; i < (size >> 1); i++)
	{
		*dst++ = ((*src) & 0xff);
		*dst++ = (((*src) >> 8) & 0xff);
		src++;
	}
}
#else
static inline void ____recorderMemcpy(____RecorderDataType* src, ____RecorderDataType* dst, uint32_t size)
{
	uint32_t i = 0;
	for (i = 0 ; i < size; i++)
		*dst++ = *src++;
}
#endif

static inline void ____triggerTriggered()
{
	extern ____Recorder ____recorder;

	if (____recorder.samplesSinceStart < ____recorder.trigger.preTriggerSamples)
		return;

	____recorder.trigger.state = ____STATE_TRIGGERED;
	int32_t idx = ____recorder.recorderbuf.head - ((____recorder.trigger.preTriggerSamples + 1) * ____recorder.samplesPackSize);
	____recorder.recorderbuf.start = idx < 0 ? ____recorder.maxActualBufferSize + idx : idx;
}

static inline void ____triggerCompare()
{
	extern ____Recorder ____recorder;

	switch (____recorder.trigger.edge)
	{
		case ____EDGE_RISING:
			if (____recorder.trigger.actValue >= ____recorder.trigger.triggerValue && ____recorder.trigger.prevValue < ____recorder.trigger.triggerValue)
				____triggerTriggered();
			break;
		case ____EDGE_FALLING:
			if (____recorder.trigger.actValue <= ____recorder.trigger.triggerValue && ____recorder.trigger.prevValue > ____recorder.trigger.triggerValue)
				____triggerTriggered();
			break;
	}
}

static inline void ____triggerCompareU32()
{
	extern ____Recorder ____recorder;

	switch (____recorder.trigger.edge)
	{
		case ____EDGE_RISING:
			if (____recorder.trigger.actValueU32 >= ____recorder.trigger.triggerValueU32 && ____recorder.trigger.prevValueU32 < ____recorder.trigger.triggerValueU32)
				____triggerTriggered();
			break;
		case ____EDGE_FALLING:
			if (____recorder.trigger.actValueU32 <= ____recorder.trigger.triggerValueU32 && ____recorder.trigger.prevValueU32 > ____recorder.trigger.triggerValueU32)
				____triggerTriggered();
			break;
	}
}

#if ____RECORDER_FLOAT_SUPPORT == 1
static inline void ____triggerCompareFloat()
{
	extern ____Recorder ____recorder;

	switch (____recorder.trigger.edge)
	{
		case ____EDGE_RISING:
			if (____recorder.trigger.actValueFloat >= ____recorder.trigger.triggerValueFloat && ____recorder.trigger.prevValueFloat < ____recorder.trigger.triggerValueFloat)
				____triggerTriggered();
			break;
		case ____EDGE_FALLING:
			if (____recorder.trigger.actValueFloat <= ____recorder.trigger.triggerValueFloat && ____recorder.trigger.prevValueFloat > ____recorder.trigger.triggerValueFloat)
				____triggerTriggered();

			break;
	}
}
#endif

static inline void ____sampleVariables()
{
	extern ____Recorder ____recorder;
	
	uint16_t i = 0;
	for (i = 0; i < ____recorder.variableCount; i++)
	{
		uint32_t currentVarSize = ____recorder.sampleList[i].size;
		____recorderMemcpy((____RecorderDataType*)____recorder.sampleList[i].address, &____recorder.recorderbuf.data[____recorder.recorderbuf.head], currentVarSize);
		____recorder.recorderbuf.head += currentVarSize;
	}

	switch (____recorder.trigger.state)
	{
		case ____STATE_SETUP:
		{
			____recorder.recorderbuf.start = 0;
			if ((____recorder.recorderbuf.head + ____recorder.samplesPackSize) > ____recorder.maxActualBufferSize)
			{
				____recorder.recorderbuf.head = 0;
				____recorder.state = ____STATE_FULL;
			}
			break;
		}
		case ____STATE_READY:
		{
			____recorder.recorderbuf.start = 0;
			if ((____recorder.recorderbuf.head + ____recorder.samplesPackSize) > ____recorder.maxActualBufferSize)
			{
				____recorder.recorderbuf.head = 0;
				____recorder.recorderbuf.start = 0;
			}

			____recorder.trigger.prevValue = ____recorder.trigger.actValue;

			switch (____recorder.trigger.type)
			{
#if ____RECORDER_C2000_SUPPORT == 0
				case ____U8:
					____recorder.trigger.actValue = (*(uint8_t*)____recorder.trigger.sourceAddress);
					____triggerCompare();
					break;
				case ____I8:
					____recorder.trigger.actValue = (*(int8_t*)____recorder.trigger.sourceAddress);
					____triggerCompare();
					break;
#endif
				case ____U16:
					____recorder.trigger.actValue = (*(uint16_t*)____recorder.trigger.sourceAddress);
					____triggerCompare();
					break;
				case ____I16:
					____recorder.trigger.actValue = (*(int16_t*)____recorder.trigger.sourceAddress);
					____triggerCompare();
					break;
				case ____U32:
					____recorder.trigger.prevValueU32 = ____recorder.trigger.actValueU32;
					____recorder.trigger.actValueU32 = (*(uint32_t*)____recorder.trigger.sourceAddress);
					____triggerCompareU32();
					break;
				case ____I32:
					____recorder.trigger.actValue = (*(int32_t*)____recorder.trigger.sourceAddress);
					____triggerCompare();
					break;
#if ____RECORDER_FLOAT_SUPPORT == 1
				case ____F32:
					____recorder.trigger.prevValueFloat = ____recorder.trigger.actValueFloat;
					____recorder.trigger.actValueFloat = (*(float*)____recorder.trigger.sourceAddress);
					____triggerCompareFloat();
					break;
#endif
				default:
					break;
			}

			break;
		}
		case ____STATE_TRIGGERED:
		{
			if ((____recorder.recorderbuf.head + ____recorder.samplesPackSize) > ____recorder.maxActualBufferSize)
			{
				____recorder.trigger.state = ____STATE_POSTTRIGGER;
				____recorder.recorderbuf.head = 0;
			}
			else if ((____recorder.recorderbuf.head + ____recorder.samplesPackSize) > ____recorder.recorderbuf.start && ____recorder.recorderbuf.head <= ____recorder.recorderbuf.start)
			{
				____recorder.trigger.state = ____STATE_POSTTRIGGER;
				____recorder.state = ____STATE_FULL;
				____recorder.recorderbuf.head = 0;
			}
			break;
		}

		case ____STATE_POSTTRIGGER:
		{
			if ((____recorder.recorderbuf.head + ____recorder.samplesPackSize) > ____recorder.recorderbuf.start)
			{
				____recorder.state = ____STATE_FULL;
				____recorder.recorderbuf.head = 0;
			}
			break;
		}
	}
}

static inline void recorderStep()
{
	extern ____Recorder ____recorder;
#if ____RECORDER_C2000_SUPPORT == 1
	extern ____RecorderSettings ____recorderSettings;
#else
	extern volatile ____RecorderSettings ____recorderSettings;
#endif
	(void)____recorderSettings.version;

	if (____STATE_RUNNING == ____recorder.state)
	{
		if (____recorder.skippedSamples >= ____recorder.samplesToSkip)
		{
			____sampleVariables();
			____recorder.samplesSinceStart++;
			____recorder.skippedSamples = 0;
			return;
		}
		____recorder.skippedSamples++;
	}
	else
	{
		____recorder.samplesSinceStart = 0;
	}
}

#endif
