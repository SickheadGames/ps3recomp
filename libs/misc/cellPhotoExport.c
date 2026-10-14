/*
 * ps3recomp - cellPhotoExport HLE implementation
 *
 * Stub. Init/term work, export returns NOT_SUPPORTED.
 */

#include "cellPhotoExport.h"
#include <stdio.h>

#include "ps3emu/logging.h"

/* Internal state */

static int s_initialized = 0;

/* API */

s32 cellPhotoExportInit(void)
{
    ps3_log("[cellPhotoExport] Init()\n");
    if (s_initialized)
        return (s32)CELL_PHOTO_EXPORT_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellPhotoExportEnd(void)
{
    ps3_log("[cellPhotoExport] End()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellPhotoExportStart(const CellPhotoExportParam* param,
                          CellPhotoExportCallback callback, void* userdata)
{
    (void)param;
    ps3_log("[cellPhotoExport] Start() - not supported\n");
    if (!s_initialized)
        return (s32)CELL_PHOTO_EXPORT_ERROR_NOT_INITIALIZED;

    if (callback)
        callback((s32)CELL_PHOTO_EXPORT_ERROR_NOT_SUPPORTED, userdata);

    return (s32)CELL_PHOTO_EXPORT_ERROR_NOT_SUPPORTED;
}
