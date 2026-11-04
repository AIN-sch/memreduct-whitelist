// Mem Reduct (whitelist edition)
// Copyright (c) 2011-2026 Henry++

#pragma once

#include "routine.h"

// global memory regions (cannot be filtered per process)
#define MT_REGION_SYSTEMFILECACHE 0x01
#define MT_REGION_MODIFIEDLIST 0x02
#define MT_REGION_STANDBYLOWPRIORITY 0x04
#define MT_REGION_COMBINEMEMORYLISTS 0x08
#define MT_REGION_REGISTRYCACHE 0x10

typedef struct _MT_STATS
{
	ULONG trimmed;
	ULONG failed;
	ULONG skipped_whitelist;
	ULONG skipped_system;
	ULONG skipped_unknown;
	ULONG protected_count;
	ULONG64 bytes_before;
	ULONG64 bytes_after;
} MT_STATS, *PMT_STATS;

typedef struct _MT_OPTIONS
{
	BOOLEAN is_skipsystem;
	BOOLEAN is_skipunknown;
	BOOLEAN is_inheritchildren;
	BOOLEAN is_protectwhitelist;
	BOOLEAN is_usecompress;
} MT_OPTIONS, *PMT_OPTIONS;

// trim working sets per process; whitelisted processes are protected instead
BOOLEAN _mt_cleanprocesses (PMT_OPTIONS options, PMT_STATS stats);

// clean global memory regions
NTSTATUS _mt_globalclean (ULONG regions);

// restore working set quotas of whitelisted processes
VOID _mt_restorewhitelist (VOID);

// failed processes of the last run
ULONG _mt_getfailurecount (VOID);

BOOLEAN _mt_getfailure (ULONG index, PULONG pid, LPWSTR name, ULONG_PTR name_length, PLONG status);
