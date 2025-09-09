// vfs_bridge.c : Minimal MicroPython VFS helpers
#include "vfs_bridge.h"
#include "py/runtime.h"
#include "py/stream.h"
#include <string.h>

bool vfs_open_rb(const char* path, mp_obj_t* out_file){
	mp_obj_t open_fn = mp_load_global(MP_QSTR_open);
	mp_obj_t args[2] = { mp_obj_new_str(path, strlen(path)), mp_obj_new_str("rb", 2) };
	mp_obj_t file = mp_call_function_n_kw(open_fn, 2, 0, args);
	if (file == MP_OBJ_NULL) return false;
	*out_file = file;
	return true;
}

int vfs_read(mp_obj_t file, uint8_t* buf, size_t nbytes){
	if (file == MP_OBJ_NULL) return -1;
	if (nbytes == 0) return 0;
	const mp_stream_p_t* stream_p = mp_get_stream(file);
	int err = 0;
	mp_uint_t r = stream_p->read(file, buf, nbytes, &err);
	if (err != 0) return -err;
	return (int)r;
}

void vfs_close(mp_obj_t* file){
	if (!file || *file == MP_OBJ_NULL) return;
	mp_obj_t close_meth = mp_load_attr(*file, MP_QSTR_close);
	mp_call_function_0(close_meth);
	*file = MP_OBJ_NULL;
}
