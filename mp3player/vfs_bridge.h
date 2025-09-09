// vfs_bridge.h
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "py/obj.h"

// Open file (binary read). Returns true on success and stores file object.
bool vfs_open_rb(const char* path, mp_obj_t* out_file);
// Read up to nbytes into buf. Returns bytes read, 0 on EOF, <0 on error.
int  vfs_read(mp_obj_t file, uint8_t* buf, size_t nbytes);
// Close file if open.
void vfs_close(mp_obj_t* file);
