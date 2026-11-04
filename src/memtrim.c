// Mem Reduct (whitelist edition)
// Copyright (c) 2011-2026 Henry++

#include "routine.h"
#include "memtrim.h"
#include "whitelist.h"

#include <psapi.h>

// undocumented: PROCESS_WORKING_SET_CONTROL (phnt ntpsapi.h:1269-1324)
#define MT_WS_CONTROL_VERSION 3
#define MT_WS_OPERATION_EMPTY 1

#define MT_WS_FLAG_EMPTY_PRIVATE_PAGES 0x01
#define MT_WS_FLAG_EMPTY_SHARED_PAGES 0x02
#define MT_WS_FLAG_EMPTY_PAGES 0x04
#define MT_WS_FLAG_COMPRESS 0x08
#define MT_WS_FLAG_STORE 0x10

// highest page priority settable from user mode
#define MT_PAGE_PRIORITY_MAX 5

#define MT_MAX_PINS 0x200
#define MT_MAX_CHAIN 0x40

typedef struct _MT_WS_CONTROL
{
	ULONG version;
	ULONG operation;
	ULONG flags;
} MT_WS_CONTROL, *PMT_WS_CONTROL;

typedef struct _MT_PIN_ENTRY
{
	HANDLE pid;
	SIZE_T minimum;
	SIZE_T maximum;
	ULONG flags;
} MT_PIN_ENTRY, *PMT_PIN_ENTRY;

typedef struct _MT_PROC_ENTRY
{
	HANDLE pid;
	HANDLE ppid;
	BOOLEAN is_system;
	BOOLEAN is_whitelist;
	BOOLEAN is_unknown;
	WCHAR name[0x40];
} MT_PROC_ENTRY, *PMT_PROC_ENTRY;

#define MT_MAX_FAILURES 0x10

typedef struct _MT_FAILURE
{
	HANDLE pid;
	LONG status;
	WCHAR name[0x40];
} MT_FAILURE, *PMT_FAILURE;

static MT_FAILURE mt_failures[MT_MAX_FAILURES] = {0};
static ULONG mt_failures_count = 0;

typedef BOOL (WINAPI *MT_GETWSIZE_FN) (HANDLE hprocess, PSIZE_T minimum, PSIZE_T maximum, PULONG flags);
typedef BOOL (WINAPI *MT_SETWSIZE_FN) (HANDLE hprocess, SIZE_T minimum, SIZE_T maximum, ULONG flags);

static MT_GETWSIZE_FN mt_getwsize = NULL;
static MT_SETWSIZE_FN mt_setwsize = NULL;
static BOOLEAN mt_is_win8 = FALSE;

static MT_PIN_ENTRY mt_pins[MT_MAX_PINS] = {0};
static ULONG mt_pins_count = 0;

// critical processes that are always skipped
static const LPCWSTR mt_skipnames[] = {
	L"System",
	L"Secure System",
	L"Registry",
	L"MemCompression",
	L"Idle",
	L"csrss.exe",
	L"wininit.exe",
	L"services.exe",
	L"winlogon.exe",
	L"smss.exe",
	L"lsass.exe",
	L"fontdrvhost.exe",
	L"dwm.exe",
	L"audiodg.exe",
};

static VOID _mt_initialize (VOID)
{
	PVOID kernel32 = GetModuleHandleW (L"kernel32.dll");

	if (kernel32)
	{
		mt_getwsize = (MT_GETWSIZE_FN)_r_sys_getprocaddress (kernel32, "GetProcessWorkingSetSizeEx", 0);
		mt_setwsize = (MT_SETWSIZE_FN)_r_sys_getprocaddress (kernel32, "SetProcessWorkingSetSizeEx", 0);
	}

	mt_is_win8 = !!(mt_getwsize && mt_setwsize && _r_sys_isosversiongreaterorequal (WINDOWS_8));
}

static BOOLEAN _mt_issystemprocess (PUNICODE_STRING image_name)
{
	R_STRINGREF sr;

	if (!image_name || image_name->Length == 0 || !image_name->Buffer)
		return FALSE;

	sr.length = image_name->Length;
	sr.buffer = image_name->Buffer;

	for (ULONG_PTR i = 0; i < RTL_NUMBER_OF (mt_skipnames); i++)
	{
		if (_r_str_isequal2 (&sr, (LPWSTR)mt_skipnames[i], TRUE))
			return TRUE;
	}

	return FALSE;
}

// record a failed process
static VOID _mt_addfailure (HANDLE pid, LPCWSTR name, LONG status)
{
	MT_FAILURE *failure;

	if (mt_failures_count >= MT_MAX_FAILURES)
		return;

	failure = &mt_failures[mt_failures_count];

	failure->pid = pid;
	failure->status = status;

	if (name && name[0] != UNICODE_NULL)
		_r_str_copy (failure->name, RTL_NUMBER_OF (failure->name), name);
	else
		_r_str_printf (failure->name, RTL_NUMBER_OF (failure->name), L"(pid %lu)", (ULONG)(ULONG_PTR)pid);

	mt_failures_count++;
}

ULONG _mt_getfailurecount (VOID)
{
	return mt_failures_count;
}

BOOLEAN _mt_getfailure (ULONG index, PULONG pid, LPWSTR name, ULONG_PTR name_length, PLONG status)
{
	MT_FAILURE *failure;

	if (index >= mt_failures_count)
		return FALSE;

	failure = &mt_failures[index];

	if (pid)
		*pid = (ULONG)(ULONG_PTR)failure->pid;

	if (name)
		_r_str_copy (name, name_length, failure->name);

	if (status)
		*status = failure->status;

	return TRUE;
}

// trim the working set of one process
static BOOLEAN _mt_trimprocess (HANDLE hprocess, BOOLEAN is_usecompress)
{
	NTSTATUS status;

	if (is_usecompress)
	{
		MT_WS_CONTROL control = {0};

		control.version = MT_WS_CONTROL_VERSION;
		control.operation = MT_WS_OPERATION_EMPTY;
		control.flags = MT_WS_FLAG_COMPRESS | MT_WS_FLAG_STORE;

		status = NtSetInformationProcess (hprocess, ProcessWorkingSetControl, &control, sizeof (control));

		if (NT_SUCCESS (status))
			return TRUE;
	}

	{
		QUOTA_LIMITS_EX limits = {0};

		limits.MinimumWorkingSetSize = (SIZE_T)-1;
		limits.MaximumWorkingSetSize = (SIZE_T)-1;

		status = NtSetInformationProcess (hprocess, ProcessQuotaLimits, &limits, sizeof (limits));

		if (NT_SUCCESS (status))
			return TRUE;
	}

	return !!EmptyWorkingSet (hprocess);
}

// whitelisted: max page priority + hard working set floor
static VOID _mt_pinprocess (HANDLE pid, HANDLE hprocess)
{
	PROCESS_MEMORY_COUNTERS counters = {0};
	PAGE_PRIORITY_INFORMATION priority = {0};
	SIZE_T minimum = 0, maximum = 0;
	ULONG flags = 0;

	if (mt_pins_count >= MT_MAX_PINS)
		return;

	priority.PagePriority = MT_PAGE_PRIORITY_MAX;

	NtSetInformationProcess (hprocess, ProcessPagePriority, &priority, sizeof (priority));

	if (!mt_is_win8)
		return;

	if (!mt_getwsize (hprocess, &minimum, &maximum, &flags))
		return;

	counters.cb = sizeof (counters);

	if (!GetProcessMemoryInfo (hprocess, &counters, sizeof (counters)))
		return;

	if (!mt_setwsize (hprocess, (SIZE_T)counters.WorkingSetSize, maximum, flags | QUOTA_LIMITS_HARDWS_MIN_ENABLE))
		return;

	mt_pins[mt_pins_count].pid = pid;
	mt_pins[mt_pins_count].minimum = minimum;
	mt_pins[mt_pins_count].maximum = maximum;
	mt_pins[mt_pins_count].flags = flags;

	mt_pins_count++;
}

static VOID _mt_unpinprocesses (VOID)
{
	for (ULONG i = 0; i < mt_pins_count; i++)
	{
		HANDLE hprocess = NULL;

		if (!NT_SUCCESS (_r_sys_openprocess (mt_pins[i].pid, PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, &hprocess)))
			continue;

		mt_setwsize (hprocess, mt_pins[i].minimum, mt_pins[i].maximum, mt_pins[i].flags);

		NtClose (hprocess);
	}

	mt_pins_count = 0;
}

BOOLEAN _mt_cleanprocesses (PMT_OPTIONS options, PMT_STATS stats)
{
	PSYSTEM_PROCESS_INFORMATION buffer = NULL;
	PSYSTEM_PROCESS_INFORMATION entry;
	PR_HASHTABLE pid_table = NULL;
	PR_STRING image_path;
	PMT_PROC_ENTRY pin = NULL;
	ULONG_PTR process_count = 0;
	ULONG_PTR index = 0;
	HANDLE current_pid = (HANDLE)(ULONG_PTR)GetCurrentProcessId ();

	if (!stats || !options)
		return FALSE;

	mt_failures_count = 0;

	if (!mt_getwsize)
		_mt_initialize ();

	if (!NT_SUCCESS (_r_sys_enumprocesses (&buffer)))
		return FALSE;

	for (entry = PR_FIRST_PROCESS (buffer); entry; entry = PR_NEXT_PROCESS (entry))
		process_count++;

	if (process_count == 0)
	{
		_r_mem_free (buffer);

		return FALSE;
	}

	pin = _r_mem_allocate (sizeof (MT_PROC_ENTRY) * process_count);

	RtlZeroMemory (pin, sizeof (MT_PROC_ENTRY) * process_count);

	pid_table = _r_obj_createhashtable (sizeof (ULONG_PTR), 0x100, NULL);

	// pass 1: collect pids and whitelist decision
	for (entry = PR_FIRST_PROCESS (buffer); entry; entry = PR_NEXT_PROCESS (entry))
	{
		R_STRINGREF path_sr;
		ULONG_PTR value;

		pin[index].pid = entry->UniqueProcessId;
		pin[index].ppid = entry->InheritedFromUniqueProcessId;
		pin[index].is_system = _mt_issystemprocess (&entry->ImageName);
		pin[index].is_whitelist = FALSE;
		pin[index].is_unknown = TRUE;
		pin[index].name[0] = UNICODE_NULL;

		if (entry->ImageName.Buffer && entry->ImageName.Length > 0)
		{
			ULONG_PTR name_length = entry->ImageName.Length / sizeof (WCHAR);

			if (name_length >= RTL_NUMBER_OF (pin[index].name))
				name_length = RTL_NUMBER_OF (pin[index].name) - 1;

			RtlCopyMemory (pin[index].name, entry->ImageName.Buffer, name_length * sizeof (WCHAR));

			pin[index].name[name_length] = UNICODE_NULL;
		}

		if (entry->UniqueProcessId == current_pid)
			pin[index].is_system = TRUE;

		image_path = NULL;

		if (NT_SUCCESS (_r_sys_getprocessimagepathbyid (entry->UniqueProcessId, TRUE, &image_path)) && image_path)
		{
			path_sr.length = image_path->length;
			path_sr.buffer = image_path->buffer;

			pin[index].is_unknown = FALSE;
			pin[index].is_whitelist = _wl_ismatch (&path_sr);

			_r_obj_dereference (image_path);
		}

		value = index + 1;

		_r_obj_addhashtableitem (pid_table, (ULONG_PTR)entry->UniqueProcessId, &value);

		index++;
	}

	// whitelist inheritance through the parent chain
	if (options->is_inheritchildren)
	{
		for (ULONG_PTR i = 0; i < process_count; i++)
		{
			HANDLE pid = pin[i].ppid;

			if (pin[i].is_whitelist)
				continue;

			for (ULONG_PTR hop = 0; hop < MT_MAX_CHAIN; hop++)
			{
				PVOID found;
				ULONG_PTR parent_index;

				if (!pid || pid == (HANDLE)(ULONG_PTR)4)
					break;

				found = _r_obj_findhashtable (pid_table, (ULONG_PTR)pid);

				if (!found)
					break;

				parent_index = *(PULONG_PTR)found - 1;

				if (parent_index >= process_count)
					break;

				if (pin[parent_index].is_whitelist)
				{
					pin[i].is_whitelist = TRUE;

					break;
				}

				pid = pin[parent_index].ppid;
			}
		}
	}

	// pass 2: trim or protect
	for (ULONG_PTR i = 0; i < process_count; i++)
	{
		HANDLE hprocess = NULL;
		ACCESS_MASK access;
		LONG last_status;

		if (pin[i].pid == current_pid || pin[i].pid == NULL || pin[i].pid == (HANDLE)(ULONG_PTR)4)
			continue;

		if (pin[i].is_whitelist)
		{
			stats->skipped_whitelist++;

			if (!options->is_protectwhitelist)
				continue;

			access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA;

			if (!NT_SUCCESS (_r_sys_openprocess (pin[i].pid, access, &hprocess)))
			{
				access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA;

				if (!NT_SUCCESS (_r_sys_openprocess (pin[i].pid, access, &hprocess)))
					continue;
			}

			_mt_pinprocess (pin[i].pid, hprocess);

			stats->protected_count++;

			NtClose (hprocess);

			continue;
		}

		if (options->is_skipsystem && pin[i].is_system)
		{
			stats->skipped_system++;

			continue;
		}

		if (options->is_skipunknown && pin[i].is_unknown)
		{
			stats->skipped_unknown++;

			continue;
		}

		access = PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA | PROCESS_VM_READ;

		last_status = _r_sys_openprocess (pin[i].pid, access, &hprocess);

		if (!NT_SUCCESS (last_status))
		{
			access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA;

			last_status = _r_sys_openprocess (pin[i].pid, access, &hprocess);

			if (!NT_SUCCESS (last_status))
			{
				_mt_addfailure (pin[i].pid, pin[i].name, last_status);

				stats->failed++;

				continue;
			}
		}

		if (_mt_trimprocess (hprocess, options->is_usecompress))
		{
			stats->trimmed++;
		}
		else
		{
			_mt_addfailure (pin[i].pid, pin[i].name, 0);

			stats->failed++;
		}

		NtClose (hprocess);
	}

	_r_obj_dereference (pid_table);

	_r_mem_free (pin);

	_r_mem_free (buffer);

	return TRUE;
}

NTSTATUS _mt_globalclean (ULONG regions)
{
	MEMORY_COMBINE_INFORMATION_EX combine_info = {0};
	SYSTEM_FILECACHE_INFORMATION file_cache = {0};
	SYSTEM_MEMORY_LIST_COMMAND command;
	NTSTATUS status = STATUS_SUCCESS;

	if ((regions & MT_REGION_SYSTEMFILECACHE) == MT_REGION_SYSTEMFILECACHE)
	{
		file_cache.MinimumWorkingSet = (SIZE_T)-1;
		file_cache.MaximumWorkingSet = (SIZE_T)-1;

		status = NtSetSystemInformation (SystemFileCacheInformationEx, &file_cache, sizeof (file_cache));
	}

	if ((regions & MT_REGION_MODIFIEDLIST) == MT_REGION_MODIFIEDLIST)
	{
		command = MemoryFlushModifiedList;

		status = NtSetSystemInformation (SystemMemoryListInformation, &command, sizeof (command));
	}

	if ((regions & MT_REGION_STANDBYLOWPRIORITY) == MT_REGION_STANDBYLOWPRIORITY)
	{
		command = MemoryPurgeLowPriorityStandbyList;

		status = NtSetSystemInformation (SystemMemoryListInformation, &command, sizeof (command));
	}

	if ((regions & MT_REGION_COMBINEMEMORYLISTS) == MT_REGION_COMBINEMEMORYLISTS)
	{
		if (_r_sys_isosversiongreaterorequal (WINDOWS_10))
			status = NtSetSystemInformation (SystemCombinePhysicalMemoryInformation, &combine_info, sizeof (combine_info));
	}

	if ((regions & MT_REGION_REGISTRYCACHE) == MT_REGION_REGISTRYCACHE)
	{
		if (_r_sys_isosversiongreaterorequal (WINDOWS_8_1))
			status = NtSetSystemInformation (SystemRegistryReconciliationInformation, NULL, 0);
	}

	return status;
}

VOID _mt_restorewhitelist (VOID)
{
	_mt_unpinprocesses ();
}
