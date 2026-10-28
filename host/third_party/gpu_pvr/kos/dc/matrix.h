#ifndef __DC_MATRIX_H
#define __DC_MATRIX_H

/* The SH-4 matrix unit.  pvr.c scales its vertices with screen_fw/screen_fh
 * directly and never issues an ftrv, so the loaded matrix is not consulted
 * by anything here.  It is kept so the register file has a defined value if
 * a future path does read it. */
typedef float matrix_t[4][4];

static matrix_t kos_shim_cur_matrix;

static inline void mat_load(const matrix_t *m)
{
	__builtin_memcpy(kos_shim_cur_matrix, m, sizeof(matrix_t));
}

static inline void mat_store(matrix_t *m)
{
	__builtin_memcpy(m, kos_shim_cur_matrix, sizeof(matrix_t));
}
#endif
