/*
 * ps3recomp - cellVideoUpload HLE implementation
 *
 * Stub. Init/term work, upload returns NOT_SUPPORTED.
 */

#include "cellVideoUpload.h"
#include <stdio.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */

#include "ps3emu/logging.h"

/* Internal state */

static int s_initialized = 0;

/* API */

s32 cellVideoUploadInit(void)
{
    ps3_log("[cellVideoUpload] Init()\n");
    if (s_initialized)
        return (s32)CELL_VIDEO_UPLOAD_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellVideoUploadTerm(void)
{
    ps3_log("[cellVideoUpload] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellVideoUploadStart(const CellVideoUploadParam* param,
                           CellVideoUploadCallback callback, void* arg)
{
    (void)param; (void)callback; (void)arg;
    ps3_log("[cellVideoUpload] Start() - not supported\n");
    if (!s_initialized) return (s32)CELL_VIDEO_UPLOAD_ERROR_NOT_INITIALIZED;
    return (s32)CELL_VIDEO_UPLOAD_ERROR_NOT_SUPPORTED;
}

s32 cellVideoUploadCancel(void)
{
    if (!s_initialized) return (s32)CELL_VIDEO_UPLOAD_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellVideoUploadGetStatus(s32* status)
{
    if (!s_initialized) return (s32)CELL_VIDEO_UPLOAD_ERROR_NOT_INITIALIZED;
    if (!status) return (s32)CELL_VIDEO_UPLOAD_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)status, (u32)0); /* idle */
    return CELL_OK;
}
