// Mem Reduct (whitelist edition)
// Copyright (c) 2011-2026 Henry++

#include "routine.h"

#include "app.h"
#include "rapp.h"
#include "resource.h"

#include "strings_cn.h"
#include "whitelist.h"
#include "memtrim.h"

#define TIMER_UID 1337
#define TIMER_INTERVAL 1000

#define AUTOCLEAN_COOLDOWN 30
#define AUTOCLEAN_DEFAULT_PERCENT 85
#define AUTOCLEAN_MIN_PERCENT 20
#define AUTOCLEAN_MAX_PERCENT 99

// usage must fall this many points below the threshold to re-arm
#define AUTOCLEAN_HYSTERESIS 5

// re-arm interval; doubled when the threshold is below the recommended value
#define AUTOCLEAN_REARM_INTERVAL 300
#define AUTOCLEAN_REARM_INTERVAL_LOW 600

// below this value the automatic cleanup is likely to fire too often
#define AUTOCLEAN_RECOMMENDED_PERCENT 50

// tray stages: green (idle) / yellow (busy) / red (tight)
#define TRAY_WARNING_DEFAULT 70
#define TRAY_DANGER_DEFAULT 85
#define TRAY_WARNING_MIN 30
#define TRAY_WARNING_MAX 90
#define TRAY_DANGER_MIN 40
#define TRAY_DANGER_MAX 99
#define TRAY_STAGE_GAP 5

#define INI_FILENAME L"memreduct.ini"
#define INI_SECTION L"main"

static const GUID GUID_TRAYICON = {0xAE9053F0, 0x8D59, 0x4803, {0x9A, 0xBB, 0x74, 0xAF, 0xE6, 0x6B, 0x5F, 0xD2}};

static WCHAR config_path[0x400] = {0};
static BOOLEAN is_exiting = FALSE;
static BOOLEAN is_trayhintshown = FALSE;
static ULONG last_clean_time = 0;
static BOOLEAN is_autoclean_armed = TRUE;
static BOOLEAN is_blockedhintshown = FALSE;

static HICON tray_icon = NULL;
static ULONG tray_percent = ULONG_MAX;

// simple decimal text to unsigned long
static ULONG _app_parseulong (LPCWSTR string)
{
	ULONG value = 0;
	ULONG_PTR i = 0;

	if (!string)
		return 0;

	while (string[i] >= L'0' && string[i] <= L'9' && i < 10)
	{
		value = (value * 10) + (ULONG)(string[i] - L'0');

		i++;
	}

	return value;
}

// configuration file path (application directory)
static VOID _app_initializeconfig (VOID)
{
	WCHAR directory[0x400];
	ULONG length;

	length = GetModuleFileNameW (NULL, directory, RTL_NUMBER_OF (directory));

	if (length == 0 || length >= RTL_NUMBER_OF (directory))
		return;

	for (ULONG i = length; i > 0; i--)
	{
		if (directory[i - 1] == L'\\')
		{
			directory[i] = UNICODE_NULL;

			break;
		}
	}

	_r_str_printf (config_path, RTL_NUMBER_OF (config_path), L"%s%s", directory, INI_FILENAME);
}

static ULONG _app_getconfig (LPCWSTR key_name, ULONG def_value)
{
	if (config_path[0] == UNICODE_NULL)
		return def_value;

	return (ULONG)GetPrivateProfileIntW (INI_SECTION, key_name, (INT)def_value, config_path);
}

static VOID _app_setconfig (LPCWSTR key_name, ULONG value)
{
	WCHAR buffer[0x20];

	if (config_path[0] == UNICODE_NULL)
		return;

	_r_str_fromulong (buffer, RTL_NUMBER_OF (buffer), value);

	WritePrivateProfileStringW (INI_SECTION, key_name, buffer, config_path);
}

// clamp the automatic cleanup threshold
static ULONG _app_getautocleanpercent (VOID)
{
	ULONG percent = _app_getconfig (L"AutoCleanPercent", AUTOCLEAN_DEFAULT_PERCENT);

	if (percent < AUTOCLEAN_MIN_PERCENT)
		percent = AUTOCLEAN_MIN_PERCENT;

	if (percent > AUTOCLEAN_MAX_PERCENT)
		percent = AUTOCLEAN_MAX_PERCENT;

	return percent;
}

// stage thresholds, validated and ordered
static VOID _app_gettraylevels (PULONG warning_level, PULONG danger_level)
{
	ULONG warning = _app_getconfig (L"TrayWarningPercent", TRAY_WARNING_DEFAULT);
	ULONG danger = _app_getconfig (L"TrayDangerPercent", TRAY_DANGER_DEFAULT);

	if (warning < TRAY_WARNING_MIN)
		warning = TRAY_WARNING_MIN;

	if (warning > TRAY_WARNING_MAX)
		warning = TRAY_WARNING_MAX;

	if (danger < TRAY_DANGER_MIN)
		danger = TRAY_DANGER_MIN;

	if (danger > TRAY_DANGER_MAX)
		danger = TRAY_DANGER_MAX;

	// keep a minimum gap between stages
	if (danger < (warning + TRAY_STAGE_GAP))
		danger = warning + TRAY_STAGE_GAP;

	if (danger > TRAY_DANGER_MAX)
	{
		danger = TRAY_DANGER_MAX;

		if (warning > (danger - TRAY_STAGE_GAP))
			warning = danger - TRAY_STAGE_GAP;
	}

	*warning_level = warning;
	*danger_level = danger;
}

// show the low-threshold warning only when it applies
static VOID _app_updateautowarn (HWND hwnd)
{
	ShowWindow (
		GetDlgItem (hwnd, IDC_AUTO_WARN),
		(_app_getautocleanpercent () < AUTOCLEAN_RECOMMENDED_PERCENT) ? SW_SHOW : SW_HIDE
	);
}

// draw the memory percentage into a tray icon (green / yellow / red stages)
static HICON _app_createicon (ULONG percent)
{
	ICONINFO icon_info = {0};
	HBRUSH hbrush;
	HBITMAP hbitmap, hbitmap_mask;
	HGDIOBJ old_bitmap;
	HFONT hfont, old_font;
	HDC hscreen, hdc;
	RECT rect;
	SIZE extent;
	WCHAR text[0x08];
	COLORREF bg_color, text_color;
	INT icon_size, font_height, text_length;
	ULONG warning_level, danger_level;
	HICON hicon;

	icon_size = GetSystemMetrics (SM_CXSMICON);

	if (icon_size <= 0)
		icon_size = 16;

	_app_gettraylevels (&warning_level, &danger_level);

	if (percent >= danger_level)
	{
		bg_color = RGB (0xE0, 0x20, 0x20);
		text_color = RGB (0xFF, 0xFF, 0xFF);
	}
	else if (percent >= warning_level)
	{
		bg_color = RGB (0xFF, 0xC0, 0x00);
		text_color = RGB (0x00, 0x00, 0x00);
	}
	else
	{
		bg_color = RGB (0x00, 0x80, 0x40);
		text_color = RGB (0xFF, 0xFF, 0xFF);
	}

	// 16px icon: cap the text at 99
	_r_str_fromulong (text, RTL_NUMBER_OF (text), (percent > 99) ? 99 : percent);

	text_length = (INT)_r_str_getlength (text);

	hscreen = GetDC (NULL);

	if (!hscreen)
		return NULL;

	hdc = CreateCompatibleDC (hscreen);
	hbitmap = CreateCompatibleBitmap (hscreen, icon_size, icon_size);

	old_bitmap = SelectObject (hdc, hbitmap);

	SetRect (&rect, 0, 0, icon_size, icon_size);

	hbrush = CreateSolidBrush (bg_color);

	FillRect (hdc, &rect, hbrush);

	DeleteObject (hbrush);

	SetBkMode (hdc, TRANSPARENT);
	SetTextColor (hdc, text_color);

	// shrink the font until the text fits the icon
	hfont = NULL;
	old_font = NULL;

	for (font_height = icon_size - 1; font_height > 5; font_height--)
	{
		hfont = CreateFontW (
			-font_height,
			0,
			0,
			0,
			FW_BOLD,
			FALSE,
			FALSE,
			FALSE,
			DEFAULT_CHARSET,
			OUT_TT_PRECIS,
			CLIP_DEFAULT_PRECIS,
			ANTIALIASED_QUALITY,
			DEFAULT_PITCH | FF_SWISS,
			L"Tahoma"
		);

		if (!hfont)
			continue;

		if (old_font)
			SelectObject (hdc, old_font);

		old_font = SelectObject (hdc, hfont);

		if (GetTextExtentPoint32W (hdc, text, text_length, &extent))
		{
			if (extent.cx <= (icon_size - 1) && extent.cy <= icon_size)
				break;
		}

		SelectObject (hdc, old_font);

		DeleteObject (hfont);

		hfont = NULL;
		old_font = NULL;
	}

	DrawTextW (hdc, text, text_length, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	if (old_font)
		SelectObject (hdc, old_font);

	if (hfont)
		DeleteObject (hfont);

	SelectObject (hdc, old_bitmap);

	hbitmap_mask = CreateBitmap (icon_size, icon_size, 1, 1, NULL);

	icon_info.fIcon = TRUE;
	icon_info.hbmColor = hbitmap;
	icon_info.hbmMask = hbitmap_mask;

	hicon = CreateIconIndirect (&icon_info);

	DeleteObject (hbitmap);
	DeleteObject (hbitmap_mask);

	DeleteDC (hdc);

	ReleaseDC (NULL, hscreen);

	return hicon;
}

// update the tray icon when the percentage changes
static VOID _app_seticon (HWND hwnd, ULONG percent)
{
	HICON hicon;
	LPCWSTR stage;
	ULONG warning_level, danger_level;

	if (percent == tray_percent && tray_icon)
		return;

	hicon = _app_createicon (percent);

	if (!hicon)
		return;

	if (tray_icon)
		DestroyIcon (tray_icon);

	tray_icon = hicon;
	tray_percent = percent;

	_app_gettraylevels (&warning_level, &danger_level);

	if (percent >= danger_level)
		stage = L_STAGE_TIGHT;
	else if (percent >= warning_level)
		stage = L_STAGE_BUSY;
	else
		stage = L_STAGE_FREE;

	_r_tray_setinfoformat (hwnd, &GUID_TRAYICON, tray_icon, L"%s: %lu%% (%s)", L_MEM_USAGE, percent, stage);
}

// localized user interface
static VOID _app_localize (HWND hwnd)
{
	SetWindowTextW (hwnd, L_APP_TITLE);

	_r_ctrl_setstring (hwnd, IDC_CLEAN, L_BTN_CLEAN);
	_r_ctrl_setstring (hwnd, IDC_AUTO_CHK, L_AUTO_ENABLE);
	_r_ctrl_setstring (hwnd, IDC_AUTO_SUFFIX, L_AUTO_SUFFIX);
	_r_ctrl_setstring (hwnd, IDC_SFC_CHK, L_SFC_ENABLE);
	_r_ctrl_setstring (hwnd, IDC_AUTO_WARN, L_AUTO_WARN);
	_r_ctrl_setstring (hwnd, IDC_WL_EDIT, L_WL_EDIT);
	_r_ctrl_setstring (hwnd, IDC_HINT, L_HINT);
}

// refresh memory and whitelist status
static VOID _app_updateui (HWND hwnd)
{
	R_MEMORY_INFO mem_info;
	WCHAR buffer[0x100], used_buffer[0x40], total_buffer[0x40], free_buffer[0x40];

	_r_sys_getmemoryinfo (&mem_info);

	_r_str_printf (buffer, RTL_NUMBER_OF (buffer), L"%s: %lu%%", L_MEM_USAGE, (ULONG)mem_info.physical_memory.percent);

	_r_ctrl_setstring (hwnd, IDC_INFO_MEM, buffer);

	_r_format_bytesize64 (used_buffer, (UINT)RTL_NUMBER_OF (used_buffer), mem_info.physical_memory.used_bytes);
	_r_format_bytesize64 (total_buffer, (UINT)RTL_NUMBER_OF (total_buffer), mem_info.physical_memory.total_bytes);
	_r_format_bytesize64 (free_buffer, (UINT)RTL_NUMBER_OF (free_buffer), mem_info.physical_memory.free_bytes);

	_r_str_printf (buffer, RTL_NUMBER_OF (buffer), L"%s %s / %s    %s %s", L_TOTAL, used_buffer, total_buffer, L_FREE, free_buffer);

	_r_ctrl_setstring (hwnd, IDC_INFO_DETAIL, buffer);

	if (!_wl_isfileexists ())
		_r_str_copy (buffer, RTL_NUMBER_OF (buffer), L_WL_MISSING);
	else if (_wl_getcount () == 0)
		_r_str_copy (buffer, RTL_NUMBER_OF (buffer), L_WL_EMPTY);
	else
		_r_str_printf (buffer, RTL_NUMBER_OF (buffer), L_WL_LOADED, (ULONG)_wl_getcount ());

	_r_ctrl_setstring (hwnd, IDC_WL_INFO, buffer);

	_app_seticon (hwnd, (ULONG)mem_info.physical_memory.percent);
}

// load persisted settings into the dialog
static VOID _app_loadsettings (HWND hwnd)
{
	WCHAR buffer[0x20];
	ULONG percent, is_autoclean;

	percent = _app_getautocleanpercent ();

	// write back the clamped value so a stale/hand-edited ini gets corrected
	_app_setconfig (L"AutoCleanPercent", percent);

	_r_str_fromulong (buffer, RTL_NUMBER_OF (buffer), percent);

	SetWindowTextW (GetDlgItem (hwnd, IDC_AUTO_VALUE_CTRL), buffer);

	is_autoclean = _app_getconfig (L"AutoClean", 0);

	SendMessageW (GetDlgItem (hwnd, IDC_AUTO_CHK), BM_SETCHECK, is_autoclean ? BST_CHECKED : BST_UNCHECKED, 0);
	SendMessageW (GetDlgItem (hwnd, IDC_SFC_CHK), BM_SETCHECK, _app_getconfig (L"CleanSystemCache", 0) ? BST_CHECKED : BST_UNCHECKED, 0);

	EnableWindow (GetDlgItem (hwnd, IDC_AUTO_VALUE_CTRL), is_autoclean ? TRUE : FALSE);

	// persist the validated tray stages and show them as a legend
	{
		ULONG warning_level, danger_level;
		WCHAR legend[0x100];

		_app_gettraylevels (&warning_level, &danger_level);

		_app_setconfig (L"TrayWarningPercent", warning_level);
		_app_setconfig (L"TrayDangerPercent", danger_level);

		_r_str_printf (
			legend,
			RTL_NUMBER_OF (legend),
			L_LEGEND,
			warning_level,
			warning_level,
			danger_level - 1,
			danger_level
		);

		_r_ctrl_setstring (hwnd, IDC_LEGEND, legend);
	}

	_app_updateautowarn (hwnd);
}

// log file path in the temporary directory
static VOID _app_getlogpath (LPWSTR buffer, ULONG_PTR buffer_length)
{
	WCHAR directory[0x400];

	if (GetTempPathW (RTL_NUMBER_OF (directory), directory) == 0)
	{
		_r_str_copy (buffer, buffer_length, L_LOG_FILENAME);

		return;
	}

	_r_str_printf (buffer, buffer_length, L"%s%s", directory, L_LOG_FILENAME);
}

// 64-bit FNV-1a, used for log de-duplication
#define LOG_HASH_BASIS 14695981039346656037ULL
#define LOG_HASH_PRIME 1099511628211ULL
#define LOG_REPEAT_LIMIT 30
#define LOG_MAX_SIZE (512 * 1024)

static ULONG64 last_entry_code = 0;
static ULONG64 last_failure_code = 0;
static ULONG suppressed_count = 0;

static ULONG64 _app_hash64 (ULONG64 hash, LPCVOID data, ULONG_PTR length)
{
	PBYTE bytes = (PBYTE)data;

	for (ULONG_PTR i = 0; i < length; i++)
	{
		hash ^= bytes[i];
		hash *= LOG_HASH_PRIME;
	}

	return hash;
}

// cleanup report; duplicate entries collapse into repeats=N
static VOID _app_writelog (PMT_STATS stats, ULONG64 freed_bytes, BOOLEAN is_auto)
{
	SYSTEMTIME time = {0};
	WCHAR path_buffer[0x400];
	WCHAR entry[0x200];
	WCHAR buffer[0x900];
	PBYTE utf8_buffer;
	HANDLE hfile;
	LARGE_INTEGER file_size = {0};
	DWORD written = 0;
	INT utf8_length;
	ULONG fail_count, i;
	ULONG64 entry_code, failure_code;
	BOOLEAN is_newfailures;
	BOOLEAN is_sameentry;

	_app_getlogpath (path_buffer, RTL_NUMBER_OF (path_buffer));

	if (!_app_getconfig (L"LogEnable", 1))
		return;

	// failure set signature
	fail_count = _mt_getfailurecount ();
	failure_code = LOG_HASH_BASIS;

	for (i = 0; i < fail_count; i++)
	{
		WCHAR name[0x40];
		ULONG pid = 0;
		LONG status = 0;

		if (!_mt_getfailure (i, &pid, name, RTL_NUMBER_OF (name), &status))
			continue;

		failure_code = _app_hash64 (failure_code, name, _r_str_getlength (name) * sizeof (WCHAR));
		failure_code = _app_hash64 (failure_code, &pid, sizeof (pid));
		failure_code = _app_hash64 (failure_code, &status, sizeof (status));
	}

	// entry body without the timestamp
	_r_str_printf (
		entry,
		RTL_NUMBER_OF (entry),
		L"mode=%s limit=%lu%% released=%lu KB trimmed=%lu skip_wl=%lu skip_sys=%lu skip_unknown=%lu protected=%lu failed=%lu",
		is_auto ? L"auto" : L"manual",
		_app_getautocleanpercent (),
		(ULONG)(freed_bytes / 1024),
		stats->trimmed,
		stats->skipped_whitelist,
		stats->skipped_system,
		stats->skipped_unknown,
		stats->protected_count,
		stats->failed
	);

	entry_code = _app_hash64 (LOG_HASH_BASIS, entry, _r_str_getlength (entry) * sizeof (WCHAR));

	is_newfailures = (failure_code != last_failure_code);
	is_sameentry = (entry_code == last_entry_code) && !is_newfailures;

	if (is_sameentry)
	{
		suppressed_count++;

		// identical line and unchanged failures: nothing worth logging
		if (suppressed_count < LOG_REPEAT_LIMIT)
			return;
	}

	GetLocalTime (&time);

	hfile = CreateFileW (path_buffer, FILE_APPEND_DATA | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hfile == INVALID_HANDLE_VALUE)
		return;

	// keep the log small
	if (GetFileSizeEx (hfile, &file_size) && file_size.QuadPart > LOG_MAX_SIZE)
	{
		NtClose (hfile);

		hfile = CreateFileW (path_buffer, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

		if (hfile == INVALID_HANDLE_VALUE)
			return;
	}

	_r_str_printf (
		buffer,
		RTL_NUMBER_OF (buffer),
		L"[%04lu-%02lu-%02lu %02lu:%02lu:%02lu] %s",
		(ULONG)time.wYear,
		(ULONG)time.wMonth,
		(ULONG)time.wDay,
		(ULONG)time.wHour,
		(ULONG)time.wMinute,
		(ULONG)time.wSecond,
		entry
	);

	if (suppressed_count > 0)
		_r_str_appendformat (buffer, RTL_NUMBER_OF (buffer), L" repeats=%lu", suppressed_count + 1);

	_r_str_append (buffer, RTL_NUMBER_OF (buffer), L"\r\n");

	if (is_newfailures)
	{
		for (i = 0; i < fail_count; i++)
		{
			WCHAR name[0x40];
			ULONG pid = 0;
			LONG status = 0;

			if (!_mt_getfailure (i, &pid, name, RTL_NUMBER_OF (name), &status))
				continue;

			_r_str_appendformat (buffer, RTL_NUMBER_OF (buffer), L"    fail: %s pid=%lu status=0x%08lX\r\n", name, pid, (ULONG)status);
		}

		last_failure_code = failure_code;
	}

	utf8_length = WideCharToMultiByte (CP_UTF8, 0, buffer, -1, NULL, 0, NULL, NULL);

	if (utf8_length > 1)
	{
		utf8_buffer = _r_mem_allocate ((ULONG_PTR)utf8_length);
		WideCharToMultiByte (CP_UTF8, 0, buffer, -1, (LPSTR)utf8_buffer, utf8_length, NULL, NULL);

		WriteFile (hfile, utf8_buffer, (DWORD)(utf8_length - 1), &written, NULL);

		_r_mem_free (utf8_buffer);
	}

	NtClose (hfile);

	last_entry_code = entry_code;
	suppressed_count = 0;
}

// perform the cleanup
static VOID _app_clean (HWND hwnd, BOOLEAN is_auto)
{
	MT_OPTIONS options = {0};
	MT_STATS stats = {0};
	R_MEMORY_INFO mem_info;
	WCHAR buffer[0x200], size_buffer[0x40];
	ULONG64 freed_bytes;
	ULONG regions;

	if (hwnd)
		_r_ctrl_setstring (hwnd, IDC_RESULT, L_CLEANING);

	// reload the whitelist on every run so edits take effect immediately
	if (!_wl_load ())
	{
		// fail closed: a missing or unreadable whitelist must never allow a full cleanup
		if (!_wl_isfileexists ())
			_wl_createtemplate ();

		_wl_lockfile ();
		_wl_load ();

		if (hwnd)
		{
			_r_ctrl_setstring (hwnd, IDC_RESULT, L_BLOCK_NOWLFILE);

			if (!is_blockedhintshown)
			{
				_r_tray_popup (hwnd, &GUID_TRAYICON, NIIF_WARNING, L_APP_TITLE, L_BLOCK_NOWLFILE);

				is_blockedhintshown = TRUE;
			}
		}

		// throttle the next attempt, otherwise a blocked cleanup retries every tick
		last_clean_time = (ULONG)_r_unixtime_now ();

		_app_updateui (hwnd);

		return;
	}

	if (_wl_getcount () == 0 && !_app_getconfig (L"AllowEmptyWhitelist", 0))
	{
		// fail closed: an empty whitelist means "everything is protected"
		if (hwnd)
		{
			_r_ctrl_setstring (hwnd, IDC_RESULT, L_BLOCK_EMPTYWL);

			if (!is_blockedhintshown)
			{
				_r_tray_popup (hwnd, &GUID_TRAYICON, NIIF_WARNING, L_APP_TITLE, L_BLOCK_EMPTYWL);

				is_blockedhintshown = TRUE;
			}
		}

		// throttle the next attempt, otherwise a blocked cleanup retries every tick
		last_clean_time = (ULONG)_r_unixtime_now ();

		_app_updateui (hwnd);

		return;
	}

	options.is_skipsystem = TRUE;
	options.is_skipunknown = TRUE;
	options.is_inheritchildren = TRUE;
	options.is_protectwhitelist = TRUE;
	options.is_usecompress = TRUE;

	_r_sys_getmemoryinfo (&mem_info);

	stats.bytes_before = mem_info.physical_memory.used_bytes;

	_mt_cleanprocesses (&options, &stats);

	regions = MT_REGION_MODIFIEDLIST | MT_REGION_STANDBYLOWPRIORITY | MT_REGION_COMBINEMEMORYLISTS | MT_REGION_REGISTRYCACHE;

	if (_app_getconfig (L"CleanSystemCache", 0))
		regions |= MT_REGION_SYSTEMFILECACHE;

	_mt_globalclean (regions);

	// restore working set quotas of whitelisted processes
	_mt_restorewhitelist ();

	_r_sys_getmemoryinfo (&mem_info);

	stats.bytes_after = mem_info.physical_memory.used_bytes;

	freed_bytes = (stats.bytes_before > stats.bytes_after) ? (stats.bytes_before - stats.bytes_after) : 0;

	_r_format_bytesize64 (size_buffer, (UINT)RTL_NUMBER_OF (size_buffer), freed_bytes);

	_r_str_printf (buffer, RTL_NUMBER_OF (buffer), L_RESULT, size_buffer, stats.trimmed, stats.skipped_whitelist, stats.failed);

	// append the failed process names for the first few failures
	{
		WCHAR fail_buffer[0x200] = {0};
		ULONG fail_count = _mt_getfailurecount ();

		for (ULONG i = 0; i < fail_count && i < 4; i++)
		{
			WCHAR name[0x40];
			ULONG pid = 0;

			if (!_mt_getfailure (i, &pid, name, RTL_NUMBER_OF (name), NULL))
				continue;

			if (fail_buffer[0] != UNICODE_NULL)
				_r_str_append (fail_buffer, RTL_NUMBER_OF (fail_buffer), L"  ");

			_r_str_appendformat (fail_buffer, RTL_NUMBER_OF (fail_buffer), L"%s(%lu)", name, pid);
		}

		if (fail_buffer[0] != UNICODE_NULL)
			_r_str_appendformat (buffer, RTL_NUMBER_OF (buffer), L"\r\n%s%s\r\n%s", L_FAIL_LABEL, fail_buffer, L_FAIL_TIP);
	}

	_app_writelog (&stats, freed_bytes, is_auto);

	if (hwnd)
	{
		_r_ctrl_setstring (hwnd, IDC_RESULT, buffer);

		if (!is_auto)
			_r_tray_popup (hwnd, &GUID_TRAYICON, NIIF_INFO, L_APP_TITLE, buffer);
	}

	last_clean_time = (ULONG)_r_unixtime_now ();

	// disarm until usage drops below threshold or the timer fires
	is_autoclean_armed = FALSE;
	is_blockedhintshown = FALSE;

	_app_updateui (hwnd);
}

// tray context menu
static VOID _app_showtraymenu (HWND hwnd)
{
	HMENU hmenu;
	POINT pt;

	hmenu = CreatePopupMenu ();

	if (!hmenu)
		return;

	AppendMenuW (hmenu, MF_STRING, IDM_TRAY_SHOW, L_TRAY_SHOW);
	AppendMenuW (hmenu, MF_STRING, IDM_TRAY_CLEAN, L_TRAY_CLEAN);
	AppendMenuW (hmenu, MF_SEPARATOR, 0, NULL);
	AppendMenuW (hmenu, MF_STRING, IDM_TRAY_EXIT, L_TRAY_EXIT);

	GetCursorPos (&pt);

	SetForegroundWindow (hwnd);

	TrackPopupMenu (hmenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);

	DestroyMenu (hmenu);
}

INT_PTR CALLBACK DlgProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
		case WM_INITDIALOG:
		{
			ULONG privileges[] = {
				SE_DEBUG_PRIVILEGE,
				SE_PROF_SINGLE_PROCESS_PRIVILEGE,
				SE_INCREASE_QUOTA_PRIVILEGE,
			};

			_r_app_sethwnd (hwnd);

			_app_initializeconfig ();

			_r_sys_setprocessprivilege (NtCurrentProcess (), privileges, (ULONG)RTL_NUMBER_OF (privileges), TRUE);

			_app_localize (hwnd);

			if (!_wl_isfileexists ())
				_wl_createtemplate ();

			_wl_lockfile ();
			_wl_load ();

			_app_loadsettings (hwnd);

			SetTimer (hwnd, TIMER_UID, TIMER_INTERVAL, NULL);

			{
				R_MEMORY_INFO mem_info;

				_r_sys_getmemoryinfo (&mem_info);

				tray_percent = (ULONG)mem_info.physical_memory.percent;
				tray_icon = _app_createicon (tray_percent);
			}

			_r_tray_create (hwnd, &GUID_TRAYICON, RM_TRAYICON, tray_icon, L_TRAY_TIP, FALSE);

			_app_updateui (hwnd);

			break;
		}

		case RM_TASKBARCREATED:
		{
			if (tray_icon)
				DestroyIcon (tray_icon);

			tray_icon = _app_createicon (tray_percent == ULONG_MAX ? 0 : tray_percent);

			_r_tray_create (hwnd, &GUID_TRAYICON, RM_TRAYICON, tray_icon, L_TRAY_TIP, FALSE);

			break;
		}

		case WM_TIMER:
		{
			R_MEMORY_INFO mem_info;
			LONG64 now;
			ULONG percent, limit, rearm_interval;

			if (wparam != TIMER_UID)
				break;

			_app_updateui (hwnd);

			if (!SendMessageW (GetDlgItem (hwnd, IDC_AUTO_CHK), BM_GETCHECK, 0, 0))
				break;

			_r_sys_getmemoryinfo (&mem_info);

			percent = (ULONG)mem_info.physical_memory.percent;
			limit = _app_getautocleanpercent ();
			now = _r_unixtime_now ();

			rearm_interval = (limit < AUTOCLEAN_RECOMMENDED_PERCENT) ? AUTOCLEAN_REARM_INTERVAL_LOW : AUTOCLEAN_REARM_INTERVAL;

			// re-arm only after usage drops below threshold - hysteresis
			if (percent <= (limit - AUTOCLEAN_HYSTERESIS))
			{
				is_autoclean_armed = TRUE;
			}
			else if (!is_autoclean_armed && (now - (LONG64)last_clean_time) >= (LONG64)rearm_interval)
			{
				// safety valve: bounded rate when usage never drops
				is_autoclean_armed = TRUE;
			}

			if (!is_autoclean_armed)
				break;

			if (percent < limit)
				break;

			if ((now - (LONG64)last_clean_time) < AUTOCLEAN_COOLDOWN)
				break;

			_app_clean (hwnd, TRUE);

			break;
		}

		case WM_COMMAND:
		{
			INT ctrl_id = LOWORD (wparam);
			INT notify_code = HIWORD (wparam);

			switch (ctrl_id)
			{
				case IDC_CLEAN:
				{
					_app_clean (hwnd, FALSE);

					break;
				}

				case IDC_AUTO_CHK:
				{
					BOOLEAN is_checked = !!SendMessageW (GetDlgItem (hwnd, IDC_AUTO_CHK), BM_GETCHECK, 0, 0);

					_app_setconfig (L"AutoClean", is_checked ? 1 : 0);

					EnableWindow (GetDlgItem (hwnd, IDC_AUTO_VALUE_CTRL), is_checked ? TRUE : FALSE);

					break;
				}

				case IDC_AUTO_VALUE_CTRL:
				{
					WCHAR buffer[0x20];
					ULONG percent;

					if (notify_code == EN_KILLFOCUS)
					{
						// normalize the field to the effective (clamped) value
						_r_str_fromulong (buffer, RTL_NUMBER_OF (buffer), _app_getautocleanpercent ());

						SetWindowTextW (GetDlgItem (hwnd, IDC_AUTO_VALUE_CTRL), buffer);

						break;
					}

					if (notify_code != EN_CHANGE)
						break;

					GetWindowTextW (GetDlgItem (hwnd, IDC_AUTO_VALUE_CTRL), buffer, RTL_NUMBER_OF (buffer));

					percent = _app_parseulong (buffer);

					if (percent >= AUTOCLEAN_MIN_PERCENT && percent <= AUTOCLEAN_MAX_PERCENT)
					{
						_app_setconfig (L"AutoCleanPercent", percent);

						// a new threshold re-arms the automatic cleanup
						is_autoclean_armed = TRUE;

						_app_updateautowarn (hwnd);
					}

					break;
				}

				case IDC_SFC_CHK:
				{
					BOOLEAN is_checked = !!SendMessageW (GetDlgItem (hwnd, IDC_SFC_CHK), BM_GETCHECK, 0, 0);

					_app_setconfig (L"CleanSystemCache", is_checked ? 1 : 0);

					break;
				}

				case IDC_WL_EDIT:
				{
					LPCWSTR path;

					if (!_wl_isfileexists ())
						_wl_createtemplate ();

					path = _wl_getfilepath ();

					if (!path || ShellExecuteW (hwnd, L"open", L"notepad.exe", path, NULL, SW_SHOWNORMAL) <= (HINSTANCE)32)
						_r_show_message (hwnd, MB_OK | MB_ICONEXCLAMATION, L_APP_TITLE, L_EDIT_FAILED);

					break;
				}

				case IDM_TRAY_SHOW:
				{
					ShowWindow (hwnd, SW_SHOW);
					SetForegroundWindow (hwnd);

					break;
				}

				case IDM_TRAY_CLEAN:
				{
					_app_clean (hwnd, FALSE);

					break;
				}

				case IDM_TRAY_EXIT:
				{
					is_exiting = TRUE;

					DestroyWindow (hwnd);

					break;
				}
			}

			break;
		}

		case RM_TRAYICON:
		{
			switch (LOWORD (lparam))
			{
				case NIN_KEYSELECT:
				case WM_LBUTTONDBLCLK:
				{
					ShowWindow (hwnd, SW_SHOW);
					SetForegroundWindow (hwnd);

					break;
				}

				case WM_CONTEXTMENU:
				{
					_app_showtraymenu (hwnd);

					break;
				}
			}

			break;
		}

		case WM_CTLCOLORSTATIC:
		{
			// low-threshold warning is painted red, everything else keeps the theme
			if ((HWND)lparam == GetDlgItem (hwnd, IDC_AUTO_WARN))
			{
				HDC hdc = (HDC)wparam;

				SetBkMode (hdc, TRANSPARENT);
				SetTextColor (hdc, RGB (0xE0, 0x20, 0x20));

				return (INT_PTR)GetStockObject (HOLLOW_BRUSH);
			}

			break;
		}

		case WM_CLOSE:
		{
			if (!is_exiting)
			{
				ShowWindow (hwnd, SW_HIDE);

				if (!is_trayhintshown)
				{
					_r_tray_popup (hwnd, &GUID_TRAYICON, NIIF_INFO, L_APP_TITLE, L_TRAY_MINIMIZED);

					is_trayhintshown = TRUE;
				}

				return TRUE;
			}

			break;
		}

		case WM_DESTROY:
		{
			KillTimer (hwnd, TIMER_UID);

			_r_tray_destroy (hwnd, &GUID_TRAYICON);

			if (tray_icon)
			{
				DestroyIcon (tray_icon);

				tray_icon = NULL;
			}

			_wl_unlockfile ();

			_wl_free ();

			PostQuitMessage (0);

			break;
		}
	}

	return FALSE;
}

INT APIENTRY wWinMain (HINSTANCE hinst, HINSTANCE prev_hinst, LPWSTR cmdline, INT show_cmd)
{
	HWND hwnd;

	UNREFERENCED_PARAMETER (prev_hinst);
	UNREFERENCED_PARAMETER (cmdline);
	UNREFERENCED_PARAMETER (show_cmd);

	if (!_r_app_initialize (NULL))
		return ERROR_APP_INIT_FAILURE;

	hwnd = _r_app_createwindow (hinst, MAKEINTRESOURCEW (IDD_MAIN), MAKEINTRESOURCEW (IDI_MAIN), &DlgProc);

	if (!hwnd)
		return ERROR_APP_INIT_FAILURE;

	return (INT)_r_wnd_message_callback (hwnd, NULL);
}
