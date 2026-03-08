#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef int mimi_err_t;

enum {
    MIMI_OK = 0,
    MIMI_ERR_FAIL = -1,
    MIMI_ERR_NO_MEM = -2,
    MIMI_ERR_INVALID_ARG = -3,
    MIMI_ERR_TIMEOUT = -4,
    MIMI_ERR_NOT_FOUND = -5,
    MIMI_ERR_INVALID_STATE = -6,
    MIMI_ERR_IO = -7,
    MIMI_ERR_NOT_SUPPORTED = -8,
    MIMI_ERR_PERMISSION_DENIED = -9,
    MIMI_ERR_ALREADY_EXISTS = -10,
    MIMI_ERR_EXIT = -11,  /* Request to exit the application */
};

const char *mimi_err_to_name(mimi_err_t err);

#ifdef __cplusplus
}
#endif

