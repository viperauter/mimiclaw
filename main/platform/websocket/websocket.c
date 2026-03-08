#include "websocket.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Platform-specific implementation */
#ifdef MG_ENABLED
#include "mg_impl/websocket.c"
#else
#error "No WebSocket implementation available"
#endif

#ifdef __cplusplus
}
#endif