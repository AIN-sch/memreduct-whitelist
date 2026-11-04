// Mem Reduct (whitelist edition)
// Copyright (c) 2011-2026 Henry++

#include "routine.h"
#include "whitelist.h"

#define WL_MAX_LINE 0x800
#define WL_MAX_FILE_SIZE (4 * 1024 * 1024)

static PR_STRING wl_file_path = NULL;
static PR_LIST wl_entries = NULL;
static HANDLE wl_lock_handle = NULL;

// list cleanup callback: dereference each item
static VOID NTAPI _wl_itemfree (PVOID object_body)
{
	_r_obj_dereference_ex (object_body, 1);
}

// application directory with trailing separator
static PR_STRING _wl_getdirectory (VOID)
{
	WCHAR buffer[0x400];
	ULONG length;

	length = GetModuleFileNameW (NULL, buffer, RTL_NUMBER_OF (buffer));

	if (length == 0 || length >= RTL_NUMBER_OF (buffer))
		return NULL;

	for (ULONG i = length; i > 0; i--)
	{
		if (buffer[i - 1] == L'\\')
		{
			buffer[i] = UNICODE_NULL;

			return _r_obj_createstring (buffer);
		}
	}

	return NULL;
}

// build the full whitelist file path
static BOOLEAN _wl_getfilename (LPWSTR buffer, ULONG_PTR buffer_length)
{
	PR_STRING directory;

	directory = _wl_getdirectory ();

	if (!directory)
		return FALSE;

	_r_str_printf (buffer, buffer_length, L"%s%s", directory->buffer, WL_FILENAME);

	_r_obj_dereference (directory);

	return (buffer[0] != UNICODE_NULL);
}

// read a text file into a wide string, BOM aware
static PR_STRING _wl_readfile (LPCWSTR path)
{
	PR_STRING string = NULL;
	LARGE_INTEGER file_size;
	PBYTE raw_buffer;
	HANDLE hfile;
	DWORD read_bytes = 0;
	ULONG_PTR raw_length;
	ULONG_PTR i;

	hfile = CreateFileW (path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hfile == INVALID_HANDLE_VALUE)
		return NULL;

	if (!GetFileSizeEx (hfile, &file_size) || file_size.QuadPart <= 0 || file_size.QuadPart > WL_MAX_FILE_SIZE)
	{
		NtClose (hfile);

		return NULL;
	}

	raw_length = (ULONG_PTR)file_size.QuadPart;

	raw_buffer = _r_mem_allocate (raw_length + sizeof (WCHAR));

	if (!ReadFile (hfile, raw_buffer, (DWORD)raw_length, &read_bytes, NULL) || read_bytes == 0)
	{
		NtClose (hfile);

		_r_mem_free (raw_buffer);

		return NULL;
	}

	NtClose (hfile);

	raw_length = read_bytes;

	if (raw_length >= 2 && raw_buffer[0] == 0xFF && raw_buffer[1] == 0xFE)
	{
		// UTF-16LE
		ULONG_PTR chars = (raw_length - 2) / sizeof (WCHAR);

		RtlMoveMemory (raw_buffer, raw_buffer + 2, chars * sizeof (WCHAR));

		((LPWSTR)raw_buffer)[chars] = UNICODE_NULL;

		string = _r_obj_createstring ((LPCWSTR)raw_buffer);
	}
	else
	{
		LPSTR ansi_buffer = (LPSTR)raw_buffer;
		ULONG_PTR ansi_length = raw_length;
		INT wide_length;
		LPWSTR wide_buffer;

		if (ansi_length >= 3 && (UCHAR)ansi_buffer[0] == 0xEF && (UCHAR)ansi_buffer[1] == 0xBB && (UCHAR)ansi_buffer[2] == 0xBF)
		{
			ansi_buffer += 3;
			ansi_length -= 3;
		}

		// strict UTF-8 first, fall back to the system code page
		wide_length = MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, ansi_buffer, (INT)ansi_length, NULL, 0);

		if (wide_length <= 0)
			wide_length = MultiByteToWideChar (CP_ACP, 0, ansi_buffer, (INT)ansi_length, NULL, 0);

		if (wide_length > 0)
		{
			wide_buffer = _r_mem_allocate (((ULONG_PTR)wide_length + 1) * sizeof (WCHAR));

			if (MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, ansi_buffer, (INT)ansi_length, wide_buffer, wide_length) <= 0)
				MultiByteToWideChar (CP_ACP, 0, ansi_buffer, (INT)ansi_length, wide_buffer, wide_length);

			wide_buffer[wide_length] = UNICODE_NULL;

			string = _r_obj_createstring (wide_buffer);

			_r_mem_free (wide_buffer);
		}
	}

	_r_mem_free (raw_buffer);

	if (string)
	{
		// unify line endings
		for (i = 0; i < _r_str_getlength2 (&string->sr); i++)
		{
			if (string->buffer[i] == L'\r')
				string->buffer[i] = L'\n';
		}
	}

	return string;
}

// trim, unquote, expand vars, unify separators
static PR_STRING _wl_normalize (LPWSTR string)
{
	WCHAR buffer[WL_MAX_LINE];
	WCHAR expanded[WL_MAX_LINE];
	WCHAR temp[WL_MAX_LINE];
	LPWSTR path = string;
	ULONG_PTR length, index;
	R_STRINGREF sr;

	while (*path == L' ' || *path == L'\t')
		path++;

	length = _r_str_getlength (path);

	while (length > 0 && (path[length - 1] == L' ' || path[length - 1] == L'\t'))
	{
		path[length - 1] = UNICODE_NULL;

		length--;
	}

	if (length == 0)
		return NULL;

	// strip paired quotes
	if (path[0] == L'"' && length >= 2 && path[length - 1] == L'"')
	{
		path++;
		path[length - 2] = UNICODE_NULL;

		length -= 2;
	}

	if (length == 0 || length >= RTL_NUMBER_OF (buffer))
		return NULL;

	_r_str_copy (buffer, RTL_NUMBER_OF (buffer), path);

	// expand environment variables
	if (ExpandEnvironmentStringsW (buffer, expanded, RTL_NUMBER_OF (expanded)) != 0)
		_r_str_copy (buffer, RTL_NUMBER_OF (buffer), expanded);

	// long path prefix
	_r_obj_initializestringref (&sr, buffer);

	if (_r_str_isstartswith2 (&sr, L"\\\\?\\UNC\\", TRUE))
	{
		_r_str_copy (temp, RTL_NUMBER_OF (temp), buffer + 8);
		_r_str_copy (buffer, RTL_NUMBER_OF (buffer), L"\\\\");
		_r_str_append (buffer, RTL_NUMBER_OF (buffer), temp);
	}
	else if (_r_str_isstartswith2 (&sr, L"\\\\?\\", TRUE))
	{
		_r_str_copy (temp, RTL_NUMBER_OF (temp), buffer + 4);
		_r_str_copy (buffer, RTL_NUMBER_OF (buffer), temp);
	}

	// unify separators
	length = _r_str_getlength (buffer);

	for (index = 0; index < length; index++)
	{
		if (buffer[index] == L'/')
			buffer[index] = L'\\';
	}

	// drop trailing separators
	while (length > 0 && buffer[length - 1] == L'\\')
	{
		buffer[length - 1] = UNICODE_NULL;

		length--;
	}

	if (length == 0)
		return NULL;

	return _r_obj_createstring (buffer);
}

BOOLEAN _wl_load (VOID)
{
	PR_STRING content, entry;
	WCHAR path_buffer[0x400];
	WCHAR line_buffer[WL_MAX_LINE];
	ULONG_PTR i, line_index, content_length;

	_wl_free ();

	if (!_wl_getfilename (path_buffer, RTL_NUMBER_OF (path_buffer)))
		return FALSE;

	wl_file_path = _r_obj_createstring (path_buffer);

	content = _wl_readfile (path_buffer);

	if (!content)
		return FALSE;

	wl_entries = _r_obj_createlist (0x10, &_wl_itemfree);

	content_length = _r_str_getlength2 (&content->sr);
	line_index = 0;

	for (i = 0; i <= content_length; i++)
	{
		if (i == content_length || content->buffer[i] == L'\n')
		{
			line_buffer[line_index] = UNICODE_NULL;

			if (line_index > 0 && line_buffer[0] != L'#' && line_buffer[0] != L';')
			{
				entry = _wl_normalize (line_buffer);

				if (entry)
				{
					BOOLEAN is_duplicate = FALSE;

					for (ULONG_PTR k = 0; k < _r_obj_getlistsize (wl_entries); k++)
					{
						PR_STRING exists = _r_obj_getlistitem (wl_entries, k);

						if (exists && _r_str_isequal (&exists->sr, &entry->sr, TRUE))
						{
							is_duplicate = TRUE;

							break;
						}
					}

					if (is_duplicate)
						_r_obj_dereference (entry);
					else
						_r_obj_addlistitem (wl_entries, entry);
				}
			}

			line_index = 0;
		}
		else if (line_index < (WL_MAX_LINE - 1))
		{
			line_buffer[line_index] = content->buffer[i];

			line_index++;
		}
	}

	_r_obj_dereference (content);

	return TRUE;
}

VOID _wl_free (VOID)
{
	_r_obj_clearreference (&wl_entries);
	_r_obj_clearreference (&wl_file_path);
}

BOOLEAN _wl_ismatch (PR_STRINGREF image_path)
{
	PR_STRING entry;

	if (!wl_entries || !image_path || !image_path->buffer || image_path->length == 0)
		return FALSE;

	for (ULONG_PTR i = 0; i < _r_obj_getlistsize (wl_entries); i++)
	{
		ULONG_PTR entry_length;

		entry = _r_obj_getlistitem (wl_entries, i);

		if (!entry)
			continue;

		entry_length = _r_str_getlength2 (&entry->sr);

		if (entry_length == 0)
			continue;

		if (image_path->length == entry->length)
		{
			if (_r_str_isequal (image_path, &entry->sr, TRUE))
				return TRUE;
		}
		else if (image_path->length > entry->length)
		{
			// separator check keeps "App" from matching "App2"
			if (image_path->buffer[entry_length] == L'\\' && _r_str_isstartswith (image_path, &entry->sr, TRUE))
				return TRUE;
		}
	}

	return FALSE;
}

ULONG_PTR _wl_getcount (VOID)
{
	if (!wl_entries)
		return 0;

	return _r_obj_getlistsize (wl_entries);
}

// live existence check, no cached state
BOOLEAN _wl_isfileexists (VOID)
{
	WCHAR path_buffer[0x400];
	R_STRINGREF sr;

	if (!_wl_getfilename (path_buffer, RTL_NUMBER_OF (path_buffer)))
		return FALSE;

	_r_obj_initializestringref (&sr, path_buffer);

	return _r_fs_exists (&sr);
}

LPCWSTR _wl_getfilepath (VOID)
{
	if (!wl_file_path)
		return NULL;

	return wl_file_path->buffer;
}

BOOLEAN _wl_createtemplate (VOID)
{
	static const WCHAR template_text[] =
		L"# \u6BCF\u884C\u4E00\u4E2A\u8DEF\u5F84\uFF1B\u8BE5\u8DEF\u5F84\u4E0B\u7684\u6240\u6709\u7A0B\u5E8F\u90FD\u4E0D\u4F1A\u88AB\u6E05\u7406\uFF08\u542B\u5B50\u76EE\u5F55\u4E0E\u5B50\u8FDB\u7A0B\uFF09\r\n"
		L"# \u4EE5 # \u6216 ; \u5F00\u5934\u7684\u662F\u6CE8\u91CA\u884C\uFF0C\u53D6\u6D88\u524D\u9762\u7684 # \u5373\u4E3A\u751F\u6548\r\n"
		L"# \u652F\u6301\u73AF\u5883\u53D8\u91CF\uFF0C\u4F8B\u5982 %ProgramFiles%\\SomeApp\r\n"
		L"#\r\n"
		L"# \u793A\u4F8B\uFF08\u53D6\u6D88\u6CE8\u91CA\u5E76\u6539\u6210\u4F60\u81EA\u5DF1\u7684\u8DEF\u5F84\uFF09\uFF1A\r\n"
		L"#D:\\YouAPP\r\n"
		L"#D:\\Steam\\steamapps\\Game\\\r\n";

	WCHAR path_buffer[0x400];
	HANDLE hfile;
	DWORD written = 0;
	INT length;
	PBYTE buffer;
	ULONG_PTR buffer_size;

	if (!_wl_getfilename (path_buffer, RTL_NUMBER_OF (path_buffer)))
		return FALSE;

	// CREATE_NEW: an existing whitelist is never truncated or overwritten
	hfile = CreateFileW (path_buffer, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hfile == INVALID_HANDLE_VALUE)
		return FALSE;

	length = WideCharToMultiByte (CP_UTF8, 0, template_text, -1, NULL, 0, NULL, NULL);

	if (length <= 0)
	{
		NtClose (hfile);

		return FALSE;
	}

	buffer_size = (ULONG_PTR)length + 3;
	buffer = _r_mem_allocate (buffer_size);

	buffer[0] = 0xEF;
	buffer[1] = 0xBB;
	buffer[2] = 0xBF;

	WideCharToMultiByte (CP_UTF8, 0, template_text, -1, (LPSTR)(buffer + 3), length, NULL, NULL);

	WriteFile (hfile, buffer, (DWORD)buffer_size, &written, NULL);

	NtClose (hfile);

	_r_mem_free (buffer);

	return TRUE;
}

// hold the file open without FILE_SHARE_DELETE: editable, not deletable
BOOLEAN _wl_lockfile (VOID)
{
	WCHAR path_buffer[0x400];

	if (wl_lock_handle)
		return TRUE;

	if (!_wl_getfilename (path_buffer, RTL_NUMBER_OF (path_buffer)))
		return FALSE;

	wl_lock_handle = CreateFileW (
		path_buffer,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);

	if (wl_lock_handle == INVALID_HANDLE_VALUE)
	{
		wl_lock_handle = NULL;

		return FALSE;
	}

	return TRUE;
}

VOID _wl_unlockfile (VOID)
{
	if (wl_lock_handle)
	{
		NtClose (wl_lock_handle);

		wl_lock_handle = NULL;
	}
}

BOOLEAN _wl_islocked (VOID)
{
	return (wl_lock_handle != NULL);
}
