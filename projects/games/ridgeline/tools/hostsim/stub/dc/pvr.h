/* Host stand-in for <dc/pvr.h>: just the types the engine headers name. */
#ifndef HOSTSIM_PVR_H
#define HOSTSIM_PVR_H
#include <stdint.h>
typedef struct { uint32_t w[8]; } pvr_poly_hdr_t;
typedef int pvr_list_t;
typedef void *pvr_ptr_t;
#define PVR_LIST_OP_POLY 0
#define PVR_LIST_TR_POLY 2
#define PVR_LIST_PT_POLY 4
#endif
