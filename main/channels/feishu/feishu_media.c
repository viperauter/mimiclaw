#include "channels/feishu/feishu_media.h"

#include "platform/http/http.h"
#include "fs/fs.h"
#include "log.h"
#include "os/os.h"
#include "cJSON.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "mimi_time.h"

static const char *TAG = "feishu";

static const char *feishu_ext_from_content_type(const char *ct)
{
    if (!ct || !ct[0]) return "bin";
    /* Strip parameters after ';' */
    char tmp[96];
    size_t n = 0;
    while (ct[n] && ct[n] != ';' && n < sizeof(tmp) - 1) {
        tmp[n] = (char)tolower((unsigned char)ct[n]);
        n++;
    }
    tmp[n] = '\0';

    if (strstr(tmp, "image/jpeg")) return "jpg";
    if (strstr(tmp, "image/png")) return "png";
    if (strstr(tmp, "image/webp")) return "webp";
    if (strstr(tmp, "image/gif")) return "gif";

    if (strstr(tmp, "audio/mpeg")) return "mp3";
    if (strstr(tmp, "audio/ogg")) return "ogg";
    if (strstr(tmp, "audio/wav")) return "wav";
    if (strstr(tmp, "audio/x-wav")) return "wav";
    if (strstr(tmp, "audio/mp4")) return "m4a";
    /* Feishu voice messages often come back as audio/octet-stream, treat them as Opus */
    if (strstr(tmp, "audio/octet-stream")) return "opus";

    if (strstr(tmp, "video/mp4")) return "mp4";
    if (strstr(tmp, "application/pdf")) return "pdf";
    return "bin";
}

mimi_err_t feishu_download_to_file(const char *url,
                                   const char *out_base_path,
                                   const char *bearer_token,
                                   char *out_final_path,
                                   size_t out_final_path_sz)
{
    if (!url || !out_base_path || !bearer_token || !bearer_token[0]) return MIMI_ERR_INVALID_ARG;
    if (out_final_path && out_final_path_sz > 0) out_final_path[0] = '\0';

    char headers[1024];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\n",
             bearer_token);

    mimi_http_request_t req = {
        .method = "GET",
        .url = url,
        .headers = headers,
        .body = NULL,
        .body_len = 0,
        .timeout_ms = 30000,
    };

    mimi_http_response_t resp;
    mimi_err_t err = MIMI_ERR_IO;
    int attempt = 0;
    for (attempt = 1; attempt <= 2; attempt++) {
        memset(&resp, 0, sizeof(resp));
        err = mimi_http_exec(&req, &resp);
        if (err == MIMI_OK) break;
        MIMI_LOGW(TAG, "Feishu download attempt %d failed: err=%d url=%.96s...", attempt, err, url);
        mimi_http_response_free(&resp);
        mimi_sleep_ms(200);
    }
    if (err != MIMI_OK) {
        return err;
    }

    /* HTTP-level validation: only 2xx is a successful download. */
    if (resp.status < 200 || resp.status >= 300) {
        const char *ct = resp.content_type ? resp.content_type : "(null)";
        if (resp.body && resp.body_len > 0) {
            size_t n = resp.body_len;
            if (n > 512) n = 512;
            MIMI_LOGW(TAG, "Feishu download HTTP status=%d ct=%s url=%.128s... body=%.512s",
                      resp.status, ct, url, (const char *)resp.body);
        } else {
            MIMI_LOGW(TAG, "Feishu download HTTP status=%d ct=%s url=%.128s... (empty body)",
                      resp.status, ct, url);
        }
        mimi_http_response_free(&resp);
        return MIMI_ERR_IO;
    }

    if (!resp.body || resp.body_len == 0) {
        MIMI_LOGW(TAG, "Feishu download empty body: status=%d ct=%s url=%.96s...",
                  resp.status, resp.content_type ? resp.content_type : "(null)", url);
        mimi_http_response_free(&resp);
        return MIMI_ERR_IO;
    }

    const char *ext = feishu_ext_from_content_type(resp.content_type);
    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s.%s", out_base_path, ext);

    MIMI_LOGI(TAG, "Feishu downloaded %zu bytes (status=%d ct=%s) -> %s",
              resp.body_len,
              resp.status,
              resp.content_type ? resp.content_type : "(null)",
              final_path);

    /* Ensure output directory exists */
    char dir[256];
    strncpy(dir, final_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        (void)mimi_fs_mkdir_p(dir);
    }

    mimi_file_t *f = NULL;
    err = mimi_fs_open(final_path, "wb", &f);
    if (err != MIMI_OK) {
        mimi_http_response_free(&resp);
        return err;
    }
    const size_t expected = resp.body_len;
    size_t written = 0;
    mimi_err_t werr = mimi_fs_write(f, resp.body, expected, &written);
    (void)mimi_fs_close(f);
    mimi_http_response_free(&resp);

    if (werr != MIMI_OK) return werr;
    if (out_final_path && out_final_path_sz > 0) {
        strncpy(out_final_path, final_path, out_final_path_sz - 1);
        out_final_path[out_final_path_sz - 1] = '\0';
    }
    return (written == expected) ? MIMI_OK : MIMI_ERR_IO;
}

static mimi_err_t feishu_fs_read_all(const char *path, uint8_t **out_buf, size_t *out_len)
{
    if (!path || !out_buf || !out_len) return MIMI_ERR_INVALID_ARG;
    *out_buf = NULL;
    *out_len = 0;

    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(path, "rb", &f);
    if (err != MIMI_OK) return err;

    long end_pos = 0;
    err = mimi_fs_seek(f, 0, SEEK_END);
    if (err != MIMI_OK) {
        (void)mimi_fs_close(f);
        return err;
    }
    err = mimi_fs_tell(f, &end_pos);
    if (err != MIMI_OK || end_pos < 0) {
        (void)mimi_fs_close(f);
        return MIMI_ERR_IO;
    }
    size_t len = (size_t) end_pos;
    err = mimi_fs_seek(f, 0, SEEK_SET);
    if (err != MIMI_OK) {
        (void)mimi_fs_close(f);
        return err;
    }

    uint8_t *buf = (uint8_t *) malloc(len ? len : 1);
    if (!buf) {
        (void)mimi_fs_close(f);
        return MIMI_ERR_NO_MEM;
    }

    size_t total = 0;
    while (total < len) {
        size_t chunk = 0;
        err = mimi_fs_read(f, buf + total, len - total, &chunk);
        if (err != MIMI_OK) {
            free(buf);
            (void)mimi_fs_close(f);
            return err;
        }
        if (chunk == 0) break;
        total += chunk;
    }
    (void)mimi_fs_close(f);

    *out_buf = buf;
    *out_len = total;
    return MIMI_OK;
}

mimi_err_t feishu_upload_image_from_file(const char *file_path,
                                         const char *image_type,
                                         const char *bearer_token,
                                         char *out_image_key,
                                         size_t out_image_key_sz)
{
    if (!file_path || !bearer_token || !bearer_token[0]) return MIMI_ERR_INVALID_ARG;
    if (out_image_key && out_image_key_sz > 0) out_image_key[0] = '\0';

    uint8_t *file_data = NULL;
    size_t file_len = 0;
    mimi_err_t err = feishu_fs_read_all(file_path, &file_data, &file_len);
    if (err != MIMI_OK) return err;

    const char *boundary = "mimiclaw-feishu-img-boundary";
    const char *img_type = (image_type && image_type[0]) ? image_type : "message";

    const char *fname = strrchr(file_path, '/');
    fname = fname ? fname + 1 : file_path;
    if (!fname || !fname[0]) fname = "image";

    char part1[256];
    int n1 = snprintf(part1, sizeof(part1),
                      "--%s\r\n"
                      "Content-Disposition: form-data; name=\"image_type\"\r\n\r\n"
                      "%s\r\n",
                      boundary, img_type);

    char part2[512];
    int n2 = snprintf(part2, sizeof(part2),
                      "--%s\r\n"
                      "Content-Disposition: form-data; name=\"image\"; filename=\"%s\"\r\n"
                      "Content-Type: application/octet-stream\r\n\r\n",
                      boundary, fname);

    char tail[64];
    int n3 = snprintf(tail, sizeof(tail),
                      "\r\n--%s--\r\n",
                      boundary);

    if (n1 <= 0 || n2 <= 0 || n3 <= 0) {
        free(file_data);
        return MIMI_ERR_FAIL;
    }

    size_t body_len = (size_t) n1 + (size_t) n2 + file_len + (size_t) n3;
    uint8_t *body = (uint8_t *) malloc(body_len);
    if (!body) {
        free(file_data);
        return MIMI_ERR_NO_MEM;
    }

    size_t off = 0;
    memcpy(body + off, part1, (size_t) n1); off += (size_t) n1;
    memcpy(body + off, part2, (size_t) n2); off += (size_t) n2;
    if (file_len > 0) {
        memcpy(body + off, file_data, file_len);
        off += file_len;
    }
    memcpy(body + off, tail, (size_t) n3);

    char headers[512];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\n"
             "Content-Type: multipart/form-data; boundary=%s\r\n",
             bearer_token, boundary);

    mimi_http_request_t req = {
        .method = "POST",
        .url = "https://open.feishu.cn/open-apis/im/v1/images",
        .headers = headers,
        .body = body,
        .body_len = body_len,
        .timeout_ms = 30000,
    };

    mimi_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    err = mimi_http_exec(&req, &resp);
    free(body);
    free(file_data);
    if (err != MIMI_OK) {
        return err;
    }

    if (resp.status < 200 || resp.status >= 300 || !resp.body) {
        MIMI_LOGE(TAG, "Feishu image upload HTTP error: status=%d", resp.status);
        mimi_http_response_free(&resp);
        return MIMI_ERR_IO;
    }

    cJSON *root = cJSON_Parse((const char *) resp.body);
    if (!root) {
        MIMI_LOGE(TAG, "Feishu image upload: failed to parse JSON");
        mimi_http_response_free(&resp);
        return MIMI_ERR_FAIL;
    }

    mimi_err_t ret = MIMI_ERR_FAIL;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && code->valueint == 0) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        cJSON *image_key = data ? cJSON_GetObjectItem(data, "image_key") : NULL;
        if (cJSON_IsString(image_key) && image_key->valuestring) {
            if (out_image_key && out_image_key_sz > 0) {
                strncpy(out_image_key, image_key->valuestring, out_image_key_sz - 1);
                out_image_key[out_image_key_sz - 1] = '\0';
            }
            ret = MIMI_OK;
        } else {
            MIMI_LOGE(TAG, "Feishu image upload: missing image_key");
        }
    } else {
        int code_val = cJSON_IsNumber(code) ? code->valueint : -1;
        MIMI_LOGE(TAG, "Feishu image upload returned error code=%d body=%.256s",
                  code_val, (const char *) resp.body);
    }

    cJSON_Delete(root);
    mimi_http_response_free(&resp);
    return ret;
}

mimi_err_t feishu_upload_file_from_file(const char *file_path,
                                        const char *file_type,
                                        const char *file_name,
                                        int duration_ms,
                                        const char *bearer_token,
                                        char *out_file_key,
                                        size_t out_file_key_sz)
{
    if (!file_path || !file_type || !file_type[0] || !bearer_token || !bearer_token[0]) {
        return MIMI_ERR_INVALID_ARG;
    }
    if (out_file_key && out_file_key_sz > 0) out_file_key[0] = '\0';

    uint8_t *file_data = NULL;
    size_t file_len = 0;
    mimi_err_t err = feishu_fs_read_all(file_path, &file_data, &file_len);
    if (err != MIMI_OK) return err;

    const char *boundary = "mimiclaw-feishu-file-boundary";

    const char *fname = file_name;
    if (!fname || !fname[0]) {
        const char *b = strrchr(file_path, '/');
        fname = b ? b + 1 : file_path;
        if (!fname || !fname[0]) fname = "file";
    }

    char part1[256];
    int n1 = snprintf(part1, sizeof(part1),
                      "--%s\r\n"
                      "Content-Disposition: form-data; name=\"file_type\"\r\n\r\n"
                      "%s\r\n",
                      boundary, file_type);

    char part2[256];
    int n2 = snprintf(part2, sizeof(part2),
                      "--%s\r\n"
                      "Content-Disposition: form-data; name=\"file_name\"\r\n\r\n"
                      "%s\r\n",
                      boundary, fname);

    char part3[256];
    int n3 = 0;
    if (duration_ms > 0) {
        int seconds = duration_ms / 1000;
        if (seconds <= 0) seconds = 1;
        n3 = snprintf(part3, sizeof(part3),
                      "--%s\r\n"
                      "Content-Disposition: form-data; name=\"duration\"\r\n\r\n"
                      "%d\r\n",
                      boundary, seconds);
    }

    char part4[512];
    int n4 = snprintf(part4, sizeof(part4),
                      "--%s\r\n"
                      "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                      "Content-Type: application/octet-stream\r\n\r\n",
                      boundary, fname);

    char tail[64];
    int n5 = snprintf(tail, sizeof(tail),
                      "\r\n--%s--\r\n",
                      boundary);

    if (n1 <= 0 || n2 <= 0 || n4 <= 0 || n5 <= 0 || (duration_ms > 0 && n3 <= 0)) {
        free(file_data);
        return MIMI_ERR_FAIL;
    }

    size_t body_len = (size_t) n1 + (size_t) n2 + (size_t) n4 + file_len + (size_t) n5;
    if (n3 > 0) body_len += (size_t) n3;

    uint8_t *body = (uint8_t *) malloc(body_len);
    if (!body) {
        free(file_data);
        return MIMI_ERR_NO_MEM;
    }

    size_t off = 0;
    memcpy(body + off, part1, (size_t) n1); off += (size_t) n1;
    memcpy(body + off, part2, (size_t) n2); off += (size_t) n2;
    if (n3 > 0) {
        memcpy(body + off, part3, (size_t) n3); off += (size_t) n3;
    }
    memcpy(body + off, part4, (size_t) n4); off += (size_t) n4;
    if (file_len > 0) {
        memcpy(body + off, file_data, file_len);
        off += file_len;
    }
    memcpy(body + off, tail, (size_t) n5);

    char headers[512];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\n"
             "Content-Type: multipart/form-data; boundary=%s\r\n",
             bearer_token, boundary);

    mimi_http_request_t req = {
        .method = "POST",
        .url = "https://open.feishu.cn/open-apis/im/v1/files",
        .headers = headers,
        .body = body,
        .body_len = body_len,
        .timeout_ms = 30000,
    };

    mimi_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    err = mimi_http_exec(&req, &resp);
    free(body);
    free(file_data);
    if (err != MIMI_OK) {
        return err;
    }

    if (resp.status < 200 || resp.status >= 300 || !resp.body) {
        MIMI_LOGE(TAG, "Feishu file upload HTTP error: status=%d", resp.status);
        mimi_http_response_free(&resp);
        return MIMI_ERR_IO;
    }

    cJSON *root = cJSON_Parse((const char *) resp.body);
    if (!root) {
        MIMI_LOGE(TAG, "Feishu file upload: failed to parse JSON");
        mimi_http_response_free(&resp);
        return MIMI_ERR_FAIL;
    }

    mimi_err_t ret = MIMI_ERR_FAIL;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && code->valueint == 0) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        cJSON *file_key = data ? cJSON_GetObjectItem(data, "file_key") : NULL;
        if (cJSON_IsString(file_key) && file_key->valuestring) {
            if (out_file_key && out_file_key_sz > 0) {
                strncpy(out_file_key, file_key->valuestring, out_file_key_sz - 1);
                out_file_key[out_file_key_sz - 1] = '\0';
            }
            ret = MIMI_OK;
        } else {
            MIMI_LOGE(TAG, "Feishu file upload: missing file_key");
        }
    } else {
        int code_val = cJSON_IsNumber(code) ? code->valueint : -1;
        MIMI_LOGE(TAG, "Feishu file upload returned error code=%d body=%.256s",
                  code_val, (const char *) resp.body);
    }

    cJSON_Delete(root);
    mimi_http_response_free(&resp);
    return ret;
}

