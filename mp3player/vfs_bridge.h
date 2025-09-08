// vfs_bridge.h
#pragma once
#include <stdbool.h>
#include <stdio.h>

// Later: helpers to open/read via MicroPython VFS.
// For step 1 we don't need actual file I/O.
static inline bool vfs_file_exists(const char* path){ (void)path; return true; }
