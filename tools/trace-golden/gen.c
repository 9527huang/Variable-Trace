/*
 * Generates the golden vectors for RTEC2, and the expected decoding of each.
 *
 * Run: gen <output-directory>
 *
 * One binary per case plus one JSON saying what a correct decoder must make of
 * it. The JSON is written from the same structs that produce the bytes, so the
 * expectation cannot drift from the file it describes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtec2_writer.h"

#define MAX_RECORDS 16

typedef struct golden_case
{
	const char* name;
	rtec2_record records[MAX_RECORDS];
	unsigned count;

	/* Bytes of a deliberately cut off record appended at the end. */
	unsigned truncatedBytes;
} golden_case;

static const char* type_name(unsigned char type)
{
	switch (type)
	{
		case 0x01:
			return "EpochBegin";
		case 0x02:
			return "ClockSync";
		case 0x03:
			return "Gap";
		case 0x04:
			return "SourceStatus";
		case 0x10:
			return "TaskCreate";
		case 0x11:
			return "TaskDelete";
		case 0x13:
			return "TaskSwitchIn";
		case 0x14:
			return "TaskSwitchOut";
		case 0x16:
			return "TaskBlock";
		case 0x17:
			return "TaskWake";
		case 0x18:
			return "PriorityChange";
		case 0x20:
			return "SyncCreate";
		case 0x21:
			return "SyncDelete";
		case 0x22:
			return "WaitBegin";
		case 0x23:
			return "WaitEnd";
		case 0x24:
			return "Signal";
		case 0x25:
			return "Acquire";
		case 0x26:
			return "Release";
		case 0x30:
			return "IsrEnter";
		case 0x31:
			return "IsrExit";
		case 0x40:
			return "Fault";
		case 0x50:
			return "Alloc";
		case 0x51:
			return "Free";
		default:
			return "Unknown";
	}
}

static rtec2_record make(unsigned char type, unsigned sequence, unsigned subject, unsigned object, unsigned arg0, unsigned arg1,
                         unsigned hasTicks, unsigned ticks)
{
	rtec2_record record;

	memset(&record, 0, sizeof(record));
	record.type = type;
	record.sequence = sequence;
	record.subject = subject;
	record.object = object;
	record.arg0 = arg0;
	record.arg1 = arg1;
	record.hasTicks = hasTicks ? 1u : 0u;
	record.ticks = ticks;

	return record;
}

static unsigned char capabilitiesPayload[4];
static unsigned char faultPayload[12];

static void build_cases(golden_case* cases, int* caseCount)
{
	int at = 0;

	/* Every type once, with the fields each one is supposed to carry. */
	cases[at].name = "one-of-each";
	cases[at].count = 9;
	cases[at].records[0] = make(0x01, 1, 0, 0, 0, 0, 1, 0);
	cases[at].records[0].payload = capabilitiesPayload;
	cases[at].records[0].payloadBytes = 4;

	cases[at].records[1] = make(0x02, 2, 0, 0, 168000000u, 0, 1, 0);
	cases[at].records[2] = make(0x10, 3, 0x20001000u, 0, 3, 1, 1, 168);
	cases[at].records[3] = make(0x13, 4, 0x20001000u, 0, 0, 0, 1, 200);
	cases[at].records[4] = make(0x16, 5, 0x20001040u, 0x20002000u, 0xFFFFFFFFu, 0, 1, 240);
	cases[at].records[5] = make(0x22, 6, 0x20001040u, 0x20002000u, 100, 0, 1, 260);
	cases[at].records[6] = make(0x30, 7, 0, 0, 15, 1, 1, 300);
	cases[at].records[7] = make(0x40, 8, 0, 0, 0, 0, 1, 320);
	cases[at].records[7].payload = faultPayload;
	cases[at].records[7].payloadBytes = 12;
	cases[at].records[8] = make(0x50, 9, 0, 0, 0x20004000u, 64, 1, 400);
	at++;

	/* A session with a gap in the middle of it. */
	cases[at].name = "session";
	cases[at].count = 7;
	cases[at].records[0] = make(0x01, 1, 0, 0, 0, 0, 1, 0);
	cases[at].records[1] = make(0x02, 2, 0, 0, 1000, 0, 1, 0);
	cases[at].records[2] = make(0x13, 3, 0x20001000u, 0, 0, 0, 1, 100);
	cases[at].records[3] = make(0x13, 4, 0x20001040u, 0, 0, 0, 1, 300);
	cases[at].records[4] = make(0x03, 5, 0, 0, 12, 5, 1, 500);
	cases[at].records[5] = make(0x13, 6, 0x20001040u, 0, 0, 0, 1, 700);
	cases[at].records[6] = make(0x14, 7, 0x20001040u, 0, 0, 0, 1, 900);
	at++;

	/* The counter going over its 32 bit end. */
	cases[at].name = "wrap";
	cases[at].count = 4;
	cases[at].records[0] = make(0x02, 10, 0, 0, 1000, 0, 1, 0);
	cases[at].records[1] = make(0x13, 11, 0x20001000u, 0, 0, 0, 1, 4294967280u);
	cases[at].records[2] = make(0x13, 12, 0x20001040u, 0, 0, 0, 1, 16);
	cases[at].records[3] = make(0x13, 13, 0x20001000u, 0, 0, 0, 1, 400);
	at++;

	/* A record type this build does not know, between two it does. */
	cases[at].name = "unknown-type";
	cases[at].count = 3;
	cases[at].records[0] = make(0x13, 20, 0x20001000u, 0, 0, 0, 1, 100);
	cases[at].records[1] = make(0x7F, 21, 0x20001000u, 0, 0, 0, 1, 200);
	cases[at].records[2] = make(0x13, 22, 0x20001040u, 0, 0, 0, 1, 300);
	at++;

	/* Two good records and ten bytes of a third: the file was cut short. */
	cases[at].name = "truncated";
	cases[at].count = 2;
	cases[at].truncatedBytes = 10;
	cases[at].records[0] = make(0x13, 30, 0x20001000u, 0, 0, 0, 1, 100);
	cases[at].records[1] = make(0x13, 31, 0x20001040u, 0, 0, 0, 1, 200);
	at++;

	*caseCount = at;
}

static void write_json(FILE* out, const golden_case* golden)
{
	unsigned index = 0;
	unsigned emitted = 0;
	unsigned knownCount = 0;
	unsigned known = 0;

	for (known = 0; known < golden->count; known++)
	{
		if (strcmp(type_name(golden->records[known].type), "Unknown") != 0)
			knownCount++;
	}

	fprintf(out, "{\n  \"file\": \"%s\",\n  \"records\": [\n", golden->name);

	for (index = 0; index < golden->count; index++)
	{
		const rtec2_record* record = &golden->records[index];

		/* A record whose type this build does not know is skipped by every
		   decoder, so it is not in the expected list either - only in the
		   count below. Expecting it would quietly turn "skip" into "decode as
		   something". */
		if (strcmp(type_name(record->type), "Unknown") == 0)
			continue;

		fprintf(out,
		        "    {\"index\": %u, \"type\": %u, \"typeName\": \"%s\", \"sequence\": %u, \"subject\": %u, \"object\": %u, "
		        "\"arg0\": %u, \"arg1\": %u, \"hasTargetTicks\": %u, \"targetTicks\": %u, \"payloadBytes\": %u}%s\n",
		        emitted, (unsigned)record->type, type_name(record->type), record->sequence, record->subject, record->object,
		        record->arg0, record->arg1, record->hasTicks ? 1u : 0u, record->ticks, (unsigned)record->payloadBytes,
		        emitted + 1 == knownCount ? "" : ",");

		emitted++;
	}

	/* An unknown type is skipped and counted; it is not in `records`. */
	fprintf(out, "  ],\n  \"unknownTypes\": %u,\n  \"truncatedBytes\": %u\n}\n",
	        strcmp(golden->name, "unknown-type") == 0 ? 1u : 0u, golden->truncatedBytes);
}

int main(int argc, char** argv)
{
	golden_case cases[8];
	int caseCount = 0;
	int index = 0;

	if (argc < 2)
	{
		fprintf(stderr, "usage: gen <output-directory>\n");

		return 2;
	}

	/* Zeroed first: a case that does not set `truncatedBytes` must not inherit
	   whatever was on the stack, which is how a wrap vector once grew 251 bytes
	   of nothing at its end. */
	memset(cases, 0, sizeof(golden_case) * 8);

	/* A capability bitmap that sets rawHandle32, lifecycleEvents and
	   eventSequence, and a fault frame with CFSR and PC present. */
	capabilitiesPayload[0] = 0x17;
	capabilitiesPayload[1] = 0x00;
	capabilitiesPayload[2] = 0x00;
	capabilitiesPayload[3] = 0x00;

	faultPayload[0] = 1;     /* UsageFault */
	faultPayload[1] = 0x03;  /* hasCFSR | hasPC */
	faultPayload[2] = 0x02;  /* INVSTATE */
	faultPayload[3] = 0x00;
	faultPayload[4] = 0x10;
	faultPayload[5] = 0x20;
	faultPayload[6] = 0x00;
	faultPayload[7] = 0x08;
	memset(faultPayload + 8, 0, 4);

	build_cases(cases, &caseCount);

	for (index = 0; index < caseCount; index++)
	{
		char path[512];
		unsigned char buffer[64];
		size_t written = 0;
		unsigned record = 0;
		FILE* out = NULL;

		snprintf(path, sizeof(path), "%s/%s.bin", argv[1], cases[index].name);
		out = fopen(path, "wb");

		if (out == NULL)
		{
			fprintf(stderr, "cannot write %s\n", path);

			return 1;
		}

		for (record = 0; record < cases[index].count; record++)
		{
			written = rtec2_write(buffer, &cases[index].records[record]);
			fwrite(buffer, 1, written, out);
		}

		if (cases[index].truncatedBytes > 0)
		{
			/* The start of a record that was never finished. */
			memset(buffer, 0x13, sizeof(buffer));
			fwrite(buffer, 1, cases[index].truncatedBytes, out);
		}

		fclose(out);

		snprintf(path, sizeof(path), "%s/%s.expected.json", argv[1], cases[index].name);
		out = fopen(path, "wb");

		if (out == NULL)
		{
			fprintf(stderr, "cannot write %s\n", path);

			return 1;
		}

		write_json(out, &cases[index]);
		fclose(out);

		printf("%s: %u records\n", cases[index].name, cases[index].count);
	}

	return 0;
}
