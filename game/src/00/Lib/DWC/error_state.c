#include "00/Lib/DWC/error_state.h"

#include "00/Lib/wifi_library_work_objects.h"

s32 stDwcErrorCode;
s32 stDwcLastError;

s32 DWC_GetLastError(s32 *errorCode) {
    if (errorCode != NULL) {
        *errorCode = stDwcErrorCode;
    }
    return stDwcLastError;
}

void DWC_ClearError(void) {
    if (stDwcLastError != 11) {
        stDwcLastError = 0;
        stDwcErrorCode = 0;
    }
}

u32 DWCi_IsError(void) {
    if (stDwcLastError != 0) {
        return 1;
    }
    return 0;
}

void DWCi_SetError(s32 error, s32 errorCode) {
    if (stDwcLastError != 11) {
        stDwcLastError = error;
        stDwcErrorCode = errorCode;
    }
#ifdef PLATFORM_PORT
    {
        extern void port_log(const char *fmt, ...);
        if (error != 0) port_log("Online: error %d", (int) (errorCode < 0 ? -errorCode : errorCode));
    }
#endif
}
