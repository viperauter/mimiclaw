#pragma once

#include "mimi_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Download a Feishu media resource to a local file.
 *
 * - url: full Feishu download URL (message resource or direct file/image URL)
 * - out_base_path: base path without extension, e.g. "downloads/feishu/audio/<file_key>"
 * - bearer_token: tenant access token ("Bearer" already handled inside)
 * - out_final_path / out_final_path_sz: optional buffer to receive final path including extension
 */
mimi_err_t feishu_download_to_file(const char *url,
                                   const char *out_base_path,
                                   const char *bearer_token,
                                   char *out_final_path,
                                   size_t out_final_path_sz);

/* Upload an image file to Feishu, returning image_key. */
mimi_err_t feishu_upload_image_from_file(const char *file_path,
                                         const char *image_type,
                                         const char *bearer_token,
                                         char *out_image_key,
                                         size_t out_image_key_sz);

/* Upload an audio/file to Feishu (including voice), returning file_key. */
mimi_err_t feishu_upload_file_from_file(const char *file_path,
                                        const char *file_type,
                                        const char *file_name,
                                        int duration_ms,
                                        const char *bearer_token,
                                        char *out_file_key,
                                        size_t out_file_key_sz);

#ifdef __cplusplus
}
#endif

