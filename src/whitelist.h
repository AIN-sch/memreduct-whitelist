// Mem Reduct (whitelist edition)
// Copyright (c) 2011-2026 Henry++

#pragma once

#include "routine.h"

// whitelist file name (located in the application directory)
#define WL_FILENAME L"\u767D\u540D\u5355.txt"

// load the whitelist file, returns TRUE on success
BOOLEAN _wl_load (VOID);

// release the whitelist
VOID _wl_free (VOID);

// check whether the image path matches any whitelist entry (prefix match with separator check)
BOOLEAN _wl_ismatch (PR_STRINGREF image_path);

// number of whitelist entries
ULONG_PTR _wl_getcount (VOID);

// whether the whitelist file exists
BOOLEAN _wl_isfileexists (VOID);

// full path of the whitelist file
LPCWSTR _wl_getfilepath (VOID);

// create the default whitelist template file
BOOLEAN _wl_createtemplate (VOID);

// keep the file open: editable, not deletable
BOOLEAN _wl_lockfile (VOID);

// release the file lock
VOID _wl_unlockfile (VOID);

// whether the file lock is held
BOOLEAN _wl_islocked (VOID);
