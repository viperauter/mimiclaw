#include "mimi_err.h"

const char *mimi_err_to_name(mimi_err_t err)
{
    switch (err) {
        case MIMI_OK: return "MIMI_OK";
        case MIMI_ERR_FAIL: return "MIMI_ERR_FAIL";
        case MIMI_ERR_NO_MEM: return "MIMI_ERR_NO_MEM";
        case MIMI_ERR_INVALID_ARG: return "MIMI_ERR_INVALID_ARG";
        case MIMI_ERR_TIMEOUT: return "MIMI_ERR_TIMEOUT";
        case MIMI_ERR_NOT_FOUND: return "MIMI_ERR_NOT_FOUND";
        case MIMI_ERR_INVALID_STATE: return "MIMI_ERR_INVALID_STATE";
        case MIMI_ERR_IO: return "MIMI_ERR_IO";
        case MIMI_ERR_NOT_SUPPORTED: return "MIMI_ERR_NOT_SUPPORTED";
        case MIMI_ERR_PERMISSION_DENIED: return "MIMI_ERR_PERMISSION_DENIED";
        case MIMI_ERR_EXIT: return "MIMI_ERR_EXIT";
        default: return "MIMI_ERR_UNKNOWN";
    }
}

