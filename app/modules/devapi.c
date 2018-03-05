#include "module.h"
#include "lauxlib.h"
#include "platform.h"

#include "c_stdlib.h"
#include "vfs.h"
#include "lwip/err.h"
#include "lwip/tcp.h"
#include "misc/dyn_buf.h"
#include "misc/string_ext.h"
#include "rboot-api.h"

#define DEVAPI_VERSION "1"
#define CONTENT_LENGTH_CHUNKED (-1)
#define CONTENT_LENGTH_UNKNOWN (-2)

#define METATABLE_DEVAPI "devapi.server"

static const char http_response_200[] = "HTTP/1.1 200 OK\r\n";
static const char http_response_401[] = "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic\r\n";
static const char http_response_404[] = "HTTP/1.1 404 Not Found\r\n";
static const char http_response_500[] = "HTTP/1.1 500 Internal Error\r\n";
static const char http_headers[] = "Access-Control-Allow-Origin:*\r\nAccess-Control-Allow-Credentials:true\r\nCache-control:no-cache\r\nConnection:close\r\n";
static const char http_header_content[] = "Content-Length:{length}\r\nContent-Type:";
static const char http_header_content_chunked[] = "Transfer-Encoding:chunked\r\nContent-Type:";
static const char default_page[] = "<!DOCTYPE html><div id=\"root\"/><script src=\"https://s-pw.github.io/nodemcu-restide/bundle.js\"></script>";

typedef enum {
    PROCESSING_HTTP_REQ,
    FILE_WRITE,
    FILE_READ,
    FLASH_WRITE,
    FLASH_READ,
    SENDING_RESPONSE,
    SENDING_CHUNKED_RESPONSE,
    CALLBACK,
    RESTART,
    LUA_EXEC,
    CLOSE,
    NOT_FOUND
} api_mode;

typedef struct {
    struct tcp_pcb *pcb;

    dynbuf_t *log_buffer;
    int max_log_length;

    int *callback_chain;
    int callback_num;

    char *credentials;
} devapi_instance;

typedef struct {
    devapi_instance *api;
    int32 remaining_content_length;
    dynbuf_t *buf;
    uint32 buf_send_ptr;
    char *credentials;
    char *http_method;
    char *path;
    int file_fd;
    uint32 flash_off;
    int flash_len;
    int response_callback;
    int response_callback_param;
    api_mode mode;
    api_mode next_mode;
    rboot_write_status write_status;
    bool response_sent;
} devapi_state;

static err_t devapi_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);

static void send_chunk(devapi_state *pState, struct tcp_pcb *pPcb);

static err_t ICACHE_FLASH_ATTR send_buf(devapi_state *req, struct tcp_pcb *pcb) {
    if (req->buf_send_ptr == 0) {
        tcp_recv(pcb, NULL);
        req->next_mode = req->mode;
        req->mode = SENDING_RESPONSE;
        req->response_sent = true;
    }

    int remaining = req->buf->length - req->buf_send_ptr;
    if (remaining > 0) {
        u16_t max_buff = tcp_sndbuf(pcb);
        u16_t to_send = (uint16) LWIP_MIN(max_buff, remaining);
        tcp_write(pcb, req->buf->data + req->buf_send_ptr, to_send, 0);
        tcp_output(pcb);
        req->buf_send_ptr += to_send;
    } else {
        req->buf_send_ptr = 0;
        dynbuf_free(req->buf);
        req->mode = req->next_mode;
    }
}

static int traceback(lua_State *L) {
    if (!lua_isstring(L, 1))  /* 'message' not a string? */
        return 1;  /* keep it intact */
    lua_getfield(L, LUA_GLOBALSINDEX, "debug");
    if (!lua_istable(L, -1) && !lua_isrotable(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    lua_getfield(L, -1, "traceback");
    if (!lua_isfunction(L, -1) && !lua_islightfunction(L, -1)) {
        lua_pop(L, 2);
        return 1;
    }
    lua_pushvalue(L, 1);  /* pass error message */
    lua_pushinteger(L, 2);  /* skip this function and traceback */
    lua_call(L, 2, 1);  /* call debug.traceback */
    return 1;
}


static int docall (lua_State *L) {
    int status;
    int base = lua_gettop(L);
    lua_pushcfunction(L, traceback);  /* push traceback function */
    lua_insert(L, base);  /* put it under chunk and args */
    status = lua_pcall(L, 0, LUA_MULTRET, base);
    lua_remove(L, base);  /* remove traceback function */
    if (status != 0) lua_gc(L, LUA_GCCOLLECT, 0);
    return status;
}


static void http_set_content_length(char *http_request, int length) {
    char tmp[9];
    char* marker_ptr = c_strstr(http_request, "{length}");
    c_sprintf(tmp, "%8d", length);
    memcpy(marker_ptr, tmp, 8);
}

static void build_http_resp(dynbuf_t *buf, int code, const char *content_type, int content_length, const void *data) {
    dynbuf_free(buf);
    switch(code) {
        case 200:
            dynbuf_append_str(buf, http_response_200);
            break;
        case 401:
            dynbuf_append_str(buf, http_response_401);
            break;
        case 404:
            dynbuf_append_str(buf, http_response_404);
            break;
        case 500:
            dynbuf_append_str(buf, http_response_500);
            break;
    }
    dynbuf_append_str(buf, http_headers);
    if (content_type) {
        if (content_length == CONTENT_LENGTH_CHUNKED) {
            dynbuf_append_str(buf, http_header_content_chunked);
        } else {
            dynbuf_append_str(buf, http_header_content);
        }
        dynbuf_append_str(buf, content_type);
        dynbuf_append_str(buf, "\r\n");
    }
    if (content_length >= 0) {
        http_set_content_length(buf->data, content_length);
    }
    dynbuf_append_str(buf, "\r\n");
    if (data) {
        dynbuf_append_str(buf, data);
    }
}

static void build_http_resp_length_unknown(dynbuf_t *buf, int code, char *content_type) {
    build_http_resp(buf, code, content_type, CONTENT_LENGTH_UNKNOWN, NULL);
}

static void build_http_resp_no_content(dynbuf_t *buf, int code) {
    build_http_resp(buf, code, NULL, CONTENT_LENGTH_UNKNOWN, NULL);
}

static int report(struct tcp_pcb *pcb, devapi_state *req, lua_State *L, int status, int results) {
    dynbuf_free(req->buf);

    if (status && !lua_isnil(L, -1)) {
        const char *msg = lua_tostring(L, -1);
        if (msg == NULL) msg = "(error object is not a string)";
        build_http_resp(req->buf, 500, "text/plain", (size_t) c_strlen(msg), msg);
        send_buf(req, pcb);
        lua_pop(L, 1);
    } else {
        build_http_resp_length_unknown(req->buf, 200, "text/plain");
        size_t http_req_len =  req->buf->length;
        for(int i = results; i > 0; i--) {
            if (!lua_isnil(L, -i)) {
                const char *msg = lua_tostring(L, -i);
                if (msg == NULL) msg = "(error object is not a string)";
                if (results != i)
                    dynbuf_append_str(req->buf, "\n");
                dynbuf_append_str(req->buf, msg);
            }
        }
        lua_pop(L, results);
        http_set_content_length(req->buf->data, req->buf->length - http_req_len);
        send_buf(req, pcb);
    }
    return status;
}

static int dostring(struct tcp_pcb *pcb, devapi_state *req, lua_State *L, const char *s, size_t len) {
    int n = lua_gettop(L);
    int status = luaL_loadbuffer(L, s, len, "exec") || docall(L);
    return report(pcb, req, L, status, lua_gettop(L) - n);
}

static void ICACHE_FLASH_ATTR restart_callback(void *arg) {
    uart_div_modify(0, 80*1000000 / 115200);
    system_restart();
}

static void ICACHE_FLASH_ATTR devapi_close(void *arg, struct tcp_pcb *pcb) {
    err_t err;

    devapi_state *req = arg;
    tcp_recv(pcb, NULL);
    err = tcp_close(pcb);

    if (err != ERR_OK) {
        /* closing failed, try again later */
        tcp_recv(pcb, devapi_recv);
    } else {
        /* closing succeeded */
        tcp_arg(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        tcp_sent(pcb, NULL);
        if (req != NULL) {
            if (req->mode == RESTART) {
                os_timer_t timeout_timer;
                os_timer_setfn(&timeout_timer, (os_timer_func_t *) restart_callback, NULL);
                os_timer_arm(&timeout_timer, 300, false);
            }

            if (req->buf) {
                dynbuf_free(req->buf);
                c_free(req->buf);
            }
            if (req->credentials) {
                c_free(req->credentials);
            }
            if (req->path) {
                c_free(req->path);
            }
            if (req->http_method) {
                c_free(req->http_method);
            }
            c_free(req);
        }
    }
}

static char *get_header(dynbuf_t *buff, const char *header) {
    char *contentLenOff = c_strncasestr(buff->data, header, buff->length);
    if (contentLenOff==0) {
        return 0;
    }
    contentLenOff += strlen(header);

    while (*contentLenOff == ' ') {
        contentLenOff++;
    }
    char *contentLenOffEnd = c_strnstr(contentLenOff, "\r\n", (size_t) (buff->length - (contentLenOff - (char *)buff->data)));
    *contentLenOffEnd = '\0';
    return contentLenOff;
}

static err_t devapi_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    devapi_state *req = arg;

    if (req->mode == SENDING_RESPONSE) {
        send_buf(req, pcb);
    }
    if (req->mode == SENDING_CHUNKED_RESPONSE) {
        send_chunk(req, pcb);
    }

    if (req->mode == FILE_READ) {
        u16_t max_buff = tcp_sndbuf(pcb);
        char *data = c_malloc(max_buff);
        u16_t read = (u16_t) vfs_read(req->file_fd, data, max_buff);
        if (read > 0) {
            tcp_write(pcb, data, read, TCP_WRITE_FLAG_COPY);
        } else {
            vfs_close(req->file_fd);
            tcp_sent(pcb, NULL);
            devapi_close(req, pcb);
        }
        c_free(data);
    } else if (req->mode == FLASH_READ) {

        if (req->flash_len >= INTERNAL_FLASH_READ_UNIT_SIZE) {
            u16_t to_read = (u16_t) LWIP_MIN(tcp_sndbuf(pcb), req->flash_len);
            to_read -= to_read % INTERNAL_FLASH_READ_UNIT_SIZE;
            uint32 *data = c_malloc(to_read);
            flash_read(req->flash_off, data, to_read);
            tcp_write(pcb, data, to_read, TCP_WRITE_FLAG_COPY);
            c_free(data);

            req->flash_off += to_read;
            req->flash_len -= to_read;
        } else if (req->flash_len > 0) {
            uint32 *data = c_malloc(INTERNAL_FLASH_READ_UNIT_SIZE);
            flash_read(req->flash_off, data, INTERNAL_FLASH_READ_UNIT_SIZE);
            tcp_write(pcb, data, (u16_t) req->flash_len, TCP_WRITE_FLAG_COPY);
            c_free(data);

            req->flash_off += INTERNAL_FLASH_READ_UNIT_SIZE;
            req->flash_len -= req->flash_len;
        } else {
            tcp_sent(pcb, NULL);
            devapi_close(req, pcb);
        }
    } else if (req->mode != SENDING_RESPONSE && req->mode != SENDING_CHUNKED_RESPONSE) {
        devapi_close(req, pcb);
    }
}

static bool ICACHE_FLASH_ATTR verify_creds(devapi_state *req, struct tcp_pcb *pcb) {
    if (req->api->credentials) {
        dbg_printf(req->api->credentials);
        if (!req->credentials || c_strcmp(req->credentials, req->api->credentials) != 0) {
            build_http_resp_no_content(req->buf, 401);
            send_buf(req, pcb);
            return false;
        }
    }
    return true;
}

static err_t ICACHE_FLASH_ATTR process_data(devapi_state *req, struct tcp_pcb *pcb, void *buf, u16_t len) {
    if (req->mode == FILE_WRITE) {
        vfs_write(req->file_fd, buf, len);
    } else if (req->mode == FLASH_WRITE) {
        rboot_write_flash(&req->write_status, (uint8*) buf, len);
    } else if (req->mode == LUA_EXEC || req->mode == CALLBACK) {
        dynbuf_append(req->buf, buf, len);
    }
}

int ends_with(const char *str, const char *suffix)
{
    if (!str || !suffix)
        return 0;
    size_t lenstr = strlen(str);
    size_t lensuffix = strlen(suffix);
    if (lensuffix >  lenstr)
        return 0;
    return strncmp(str + lenstr - lensuffix, suffix, lensuffix) == 0;
}

static err_t ICACHE_FLASH_ATTR process_payload(devapi_state *req, struct tcp_pcb *pcb, char *buf, u16_t len) {
    if (req->remaining_content_length == -1) {
        dynbuf_append(req->buf, buf, len);
        char *headersEnd = c_strnstr(req->buf->data, "\r\n\r\n", req->buf->length);
        if (headersEnd != NULL) {
            headersEnd += 4;        //skip \r\n\r\n

            char *methodEnd = c_strnstr((char*) req->buf->data + 2, " ", req->buf->length - 2);

            uint32 method_len = (uint32) (methodEnd - (char*)req->buf->data);
            req->http_method = c_malloc(method_len + 1);
            c_strncpy(req->http_method, req->buf->data, method_len);
            req->http_method[method_len] = '\0';

            char *pathEnd = c_strnstr(methodEnd + 1, " ", req->buf->length);
            unsigned int path_len = (uint32) (pathEnd - methodEnd - 1);
            req->path = c_malloc(path_len + 1);
            c_strncpy(req->path, methodEnd + 1, path_len);
            req->path[path_len] = '\0';

            char *content_len = get_header(req->buf, "content-length:");
            if (content_len == NULL) {
                req->remaining_content_length = 0;
            } else {
                req->remaining_content_length = (int32) strtol(content_len, NULL, 10);
            }

            char *authorization = get_header(req->buf, "authorization:");
            if (authorization) {
                int auth_len = c_strlen(authorization);
                req->credentials = c_malloc(auth_len - 5);
                c_memcpy(req->credentials, authorization + 6, (unsigned int) auth_len - 5);
            }
            dynbuf_remove_first(req->buf, (size_t) (headersEnd - (char*) req->buf->data));
            req->remaining_content_length -= req->buf->length;

            if (c_strncmp(req->path, "/api/", 5) == 0) {
                char *path = req->path + 5;
                if (c_strncmp(path, "fs", 2) == 0 && verify_creds(req, pcb)) {
                    if (c_strlen(path) > 3) {
                        path = path + 3;

                        if (strcmp(req->http_method, "POST") == 0) {
                            req->mode = FILE_WRITE;
                            req->file_fd = vfs_open(path, "w");
                            if (req->buf->length > 0) {
                                process_data(req, pcb, req->buf->data, (u16_t) req->buf->length);
                            }
                            dynbuf_free(req->buf);
                        } else if (strcmp(req->http_method, "PUT") == 0) {
                            req->mode = FILE_WRITE;
                            req->file_fd = vfs_open(path, "a");

                            if (req->buf->length > 0) {
                                process_data(req, pcb, req->buf->data, (u16_t) req->buf->length);
                            }
                            dynbuf_free(req->buf);
                        } else if (strcmp(req->http_method, "GET") == 0) {
                            req->mode = FILE_READ;
                            req->file_fd = vfs_open(path, "r");
                            uint32_t size = vfs_size(req->file_fd);
                            build_http_resp(req->buf, 200, "application/octet-stream", (size_t) size, NULL);
                            send_buf(req, pcb);
                        } else if (strcmp(req->http_method, "DELETE") == 0) {
                            vfs_remove(path);
                        }
                    } else {
                        build_http_resp_length_unknown(req->buf, 200, "application/json");
                        size_t http_req_len =  req->buf->length;
                        dynbuf_append_str(req->buf, "{");

                        vfs_dir  *dir;
                        dir = vfs_opendir("");
                        struct vfs_stat stat;
                        bool empty = true;
                        while (vfs_readdir(dir, &stat) == VFS_RES_OK) {
                            int name_len = strlen(stat.name);
                            char *json_el = c_malloc(name_len + 8 + 4 + 1);
                            c_sprintf(json_el, "\"%s\":%d,", stat.name, stat.size);
                            dynbuf_append_str(req->buf, json_el);
                            c_free(json_el);
                            empty = false;
                        }
                        vfs_closedir(dir);

                        if (empty) {
                            dynbuf_append_str(req->buf, "}");
                        } else {
                            ((char *)req->buf->data)[req->buf->length - 1] = '}';   //replace last , with }
                        }
                        http_set_content_length(req->buf->data, req->buf->length - http_req_len);
                        send_buf(req, pcb);
                    }
                } else if (c_strncmp(path, "restart", 7) == 0 && verify_creds(req, pcb)) {
                    req->mode = RESTART;
                } else if (c_strncmp(path, "log", 3) == 0 && verify_creds(req, pcb)) {
                    if (req->api->log_buffer && req->api->log_buffer->length > 0) {
                        build_http_resp(req->buf, 200, "text/plain", (size_t) req->api->log_buffer->length, req->api->log_buffer->data);
                        dynbuf_free(req->api->log_buffer);
                        send_buf(req, pcb);
                    }
                } else if (c_strncmp(path, "exec", 4) == 0 && verify_creds(req, pcb)) {
                    req->mode = LUA_EXEC;
                } else if (c_strncmp(path, "flash", 5) == 0 && verify_creds(req, pcb)) {
                    path = path + 6;
                    if (strcmp(req->http_method, "POST") == 0) {
                        req->mode = FLASH_WRITE;
                        req->write_status = rboot_write_init((uint32) strtol(path, 0, 16));
                        if (req->buf->length > 0) {
                            process_data(req, pcb, req->buf->data, (u16_t) req->buf->length);
                        }
                        dynbuf_free(req->buf);
                    } else if (strcmp(req->http_method, "GET") == 0) {
                        req->mode = FLASH_READ;
                        char* len_off;
                        req->flash_off = (uint32) strtol(path, &len_off, 16);
                        req->flash_len = (int) strtol(len_off + 1, NULL, 16);
                        build_http_resp(req->buf, 200, "application/octet-stream", (size_t) req->flash_len, NULL);
                        send_buf(req, pcb);
                    }
                } else if (c_strncmp(path, "ping", 4) == 0) {
                    //NOP
                } else if (c_strncmp(path, "version", 7) == 0) {
                    build_http_resp(req->buf, 200, "text/plain", (size_t) c_strlen(DEVAPI_VERSION), DEVAPI_VERSION);
                    dynbuf_append(req->buf, "\0", 1);
                    send_buf(req, pcb);
                } else {
                    req->mode = CALLBACK;
                }
            } else if (c_strcmp(req->path, "/") == 0 && verify_creds(req, pcb)) {
                char *path = req->path + 1;
                req->file_fd = vfs_open("index.html", "r");
                if (req->file_fd) {
                    req->mode = FILE_READ;
                    uint32_t size = vfs_size(req->file_fd);
                    build_http_resp(req->buf, 200, "text/html", (size_t) size, NULL);
                } else {
                    build_http_resp(req->buf, 200, "text/html", (size_t) c_strlen(default_page), default_page);
                }
                send_buf(req, pcb);
            } else {
                req->mode = CALLBACK;
            }
        }
    } else {
        req->remaining_content_length -= len;
        process_data(req, pcb, buf, len);
    }
    if ( req->remaining_content_length == 0) {
        if (req->mode == FILE_WRITE) {
            vfs_close(req->file_fd);
            tcp_recv(pcb, NULL);
        } else if (req->mode == FLASH_WRITE) {
            if (req->write_status.extra_count > 0) {
                uint32 fill = 0xFFFFFFFF;
                rboot_write_flash(&req->write_status, (uint8 *) &fill, 4);
            }
        } else if (req->mode == LUA_EXEC) {
            lua_State *L = lua_getstate();
            dostring(pcb, req, L, req->buf->data, req->buf->length);
        } else if (req->mode == CALLBACK) {
            bool processed = false;
            for (int i = 0; i < req->api->callback_num; i++) {
                lua_State *L = lua_getstate();
                lua_rawgeti(L, LUA_REGISTRYINDEX, req->api->callback_chain[i]); // load the callback function
                int params = 3;
                lua_pushstring(L, req->http_method);
                lua_pushstring(L, req->path);
                lua_pushlstring(L, req->buf->data, req->buf->length); // pass data
                if (req->credentials) {
                    lua_pushstring(L, req->credentials);
                    params++;
                }

                int n = lua_gettop(L);
                lua_call(L, params, LUA_MULTRET);
                int ret_vals = lua_gettop(L) - n + 4;

                if (ret_vals > 0) {
                    int code = (int) lua_tointeger(L, -ret_vals);
                    if (ret_vals > 1) {
                        const char *content_type = lua_tostring(L, -ret_vals + 1);

                        if (lua_isfunction(L, -ret_vals + 2) || lua_islightfunction(L, -ret_vals + 2)) {
                            lua_pushvalue(L, -ret_vals + 2);  // copy argument (func) to the top of stack'
                            req->response_callback = luaL_ref(L, LUA_REGISTRYINDEX);
                            if (ret_vals > 3) {
                                lua_pushvalue(L, -ret_vals + 3);
                                req->response_callback_param = luaL_ref(L, LUA_REGISTRYINDEX);
                            }
                            req->mode = SENDING_CHUNKED_RESPONSE;
                            build_http_resp(req->buf, code, content_type, CONTENT_LENGTH_CHUNKED, NULL);
                        } else {
                            size_t data_len;
                            const char *data = lua_tolstring(L, -1, &data_len);
                            build_http_resp(req->buf, code, content_type, data_len, data);
                        }
                    } else {
                        build_http_resp_no_content(req->buf, code);
                    }

                    send_buf(req, pcb);

                    lua_pop(L, ret_vals);
                    processed = true;
                    break;
                }
            }
            if (! processed) {
                char *path = req->path + 1;
                req->file_fd = vfs_open(path, "r");
                if (req->file_fd) {
                    req->mode = FILE_READ;
                    uint32_t size = vfs_size(req->file_fd);
                    char* content_type;
                    if (ends_with(path, ".html") || ends_with(path, ".htm")) {
                        content_type = "text/html";
                    } else if (ends_with(path, ".css")) {
                        content_type = "text/css";
                    } else if (ends_with(path, ".xhtml")) {
                        content_type = "application/xhtml+xml";
                    } else if (ends_with(path, ".js")) {
                        content_type = "application/javascript";
                    } else if (ends_with(path, ".json")) {
                        content_type = "application/json";
                    } else if (ends_with(path, ".xml")) {
                        content_type = "application/xml";
                    } else if (ends_with(path, ".ico")) {
                        content_type = "image/x-icon";
                    } else if (ends_with(path, ".jpeg") || ends_with(path, ".jpg")) {
                        content_type = "image/jpeg";
                    } else if (ends_with(path, ".gif")) {
                        content_type = "image/gif";
                    } else if (ends_with(path, ".png")) {
                        content_type = "image/png";
                    } else {
                        content_type = "application/octet-stream";
                    }
                    build_http_resp(req->buf, 200, content_type, (size_t) size, NULL);
                    send_buf(req, pcb);
                } else {
                    req->mode = NOT_FOUND;
                }
            }
        }
        if (!req->response_sent) {
            build_http_resp_no_content(req->buf, req->mode == NOT_FOUND ? 404 : 200);
            send_buf(req, pcb);
        }
    }
}

static void ICACHE_FLASH_ATTR send_chunk(devapi_state *req, struct tcp_pcb *pcb) {
    lua_State *L = lua_getstate();
    lua_rawgeti(L, LUA_REGISTRYINDEX, req->response_callback);
    if (req->response_callback_param) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, req->response_callback_param);
        lua_call(L, 1, 1);
    } else {
        lua_call(L, 0, 1);
    }
    if (! lua_isnoneornil(L, -1)) {
        size_t data_len;
        const char *data = lua_tolstring(L, -1, &data_len);
        char tmp[11];
        c_sprintf(tmp, "%X\r\n", data_len);

        dynbuf_free(req->buf);
        dynbuf_append(req->buf, tmp, (size_t) c_strlen(tmp));
        dynbuf_append(req->buf, data, data_len);
        dynbuf_append(req->buf, "\r\n", 2);
        send_buf(req, pcb);
    } else {
        lua_unref(L, req->response_callback);
        if (req->response_callback_param) {
            lua_unref(L, req->response_callback_param);
        }

        dynbuf_free(req->buf);
        dynbuf_append(req->buf, "0\r\n\r\n", 5);
        send_buf(req, pcb);
        req->mode = CLOSE;
    }

    lua_pop(L, 1);
}

static err_t ICACHE_FLASH_ATTR devapi_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (err == ERR_OK && p != NULL) {
        struct pbuf *bufchain = p;

        while(bufchain != NULL) {
            process_payload((devapi_state *) arg, pcb, bufchain->payload, bufchain->len);
            bufchain = bufchain->next;
        }

        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
    } else {
        if (p != NULL) {
            tcp_recved(pcb, p->tot_len);
        }

        /* error or closed by other side */
        if (p != NULL) {
            pbuf_free(p);
        }

        /* close the connection */
        devapi_close(arg, pcb);
    }
    return ERR_OK;
}

static err_t ICACHE_FLASH_ATTR devapi_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    devapi_instance *api = (devapi_instance *) arg;
    devapi_state *req;

    LWIP_UNUSED_ARG(err);

    req = (devapi_state *) c_zalloc(sizeof(devapi_state));

    if (req == NULL) {
        return ERR_MEM;
    }
    req->api = api;
    req->buf = c_zalloc(sizeof(dynbuf_t));
    req->remaining_content_length = -1;
    req->mode = PROCESSING_HTTP_REQ;

    tcp_arg(pcb, req);
    tcp_sent(pcb, devapi_sent);
    tcp_recv(pcb, devapi_recv);
    return ERR_OK;
}

static int ICACHE_FLASH_ATTR devapi_create(lua_State *L) {
    u16_t port = (u16_t) luaL_optinteger(L, 1, 80);
    int log_buff_len = (int) luaL_optinteger(L, 2, 1000);
    const char *credentials = luaL_optstring(L, 3, NULL);

    devapi_instance *api = (devapi_instance *) lua_newuserdata(L, sizeof(devapi_instance));
    c_memset(api, 0, sizeof(devapi_instance));

    if (log_buff_len) {
        api->max_log_length = log_buff_len;
        api->log_buffer = c_zalloc(sizeof(dynbuf_t));
    }
    if (credentials) {
        size_t buf_len = strlen(credentials) + 1;
        api->credentials = c_malloc(buf_len);
        c_memcpy(api->credentials, credentials, buf_len);
    }

    api->pcb = tcp_new();
    tcp_bind(api->pcb, IP_ADDR_ANY, port);
    api->pcb = tcp_listen(api->pcb);
    tcp_arg(api->pcb, api);
    tcp_accept(api->pcb, devapi_accept);

    luaL_getmetatable(L, METATABLE_DEVAPI);
    lua_setmetatable(L, -2);

    return 1;
}

static int ICACHE_FLASH_ATTR devapi_log(lua_State *L) {
    devapi_instance *api = (devapi_instance *) luaL_checkudata(L, 1, METATABLE_DEVAPI);
    const char *msg = luaL_optstring(L, 2, NULL);

    if (msg && api->log_buffer) {
        int len = c_strlen(msg);
        int total_len = len + api->log_buffer->length;
        if (total_len > api->max_log_length) {
            int to_remove = total_len - api->max_log_length;
            if (to_remove <= api->max_log_length && len < api->max_log_length) {
                dynbuf_remove_first(api->log_buffer, (size_t) to_remove);
            } else {
                dynbuf_free(api->log_buffer);
            }
        }
        int msg_off = LWIP_MAX(len - api->max_log_length, 0);
        dynbuf_append(api->log_buffer, msg + msg_off, (size_t) LWIP_MIN(len, api->max_log_length));
    }
}

static int ICACHE_FLASH_ATTR devapi_add_callback(lua_State *L) {
    devapi_instance *api = (devapi_instance *) luaL_checkudata(L, 1, METATABLE_DEVAPI);

    if (lua_type(L, 3) != LUA_TNIL) {
        lua_pushvalue(L, 3);  // copy argument (func) to the top of stack
        api->callback_num++;
        api->callback_chain = c_realloc(api->callback_chain, api->callback_num * 4);
        api->callback_chain[api->callback_num - 1] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
}

static int ICACHE_FLASH_ATTR devapi_server_close(lua_State *L) {
    devapi_instance *api = (devapi_instance *) luaL_checkudata(L, 1, METATABLE_DEVAPI);
    if (api->pcb && tcp_close(api->pcb) != ERR_OK) {
        tcp_arg(api->pcb, NULL);
        tcp_abort(api->pcb);
    }
    api->pcb = NULL;
    for (int i = 0; i < api->callback_num; i++) {
        luaL_unref(L, LUA_REGISTRYINDEX, api->callback_chain[i]);
    }
    if (api->callback_chain) {
        c_free(api->callback_chain);
    }
    if (api->log_buffer) {
        dynbuf_free(api->log_buffer);
        api->log_buffer = NULL;
    }
    if (api->credentials) {
        c_free(api->credentials);
    }
}

static const LUA_REG_TYPE devapi_map[] = {
        {LSTRKEY("createServer"), LFUNCVAL(devapi_create)},
        {LNILKEY, LNILVAL}
};

static const LUA_REG_TYPE devapiserver_map[] = {
        {LSTRKEY("log"), LFUNCVAL(devapi_log)},
        {LSTRKEY("on"), LFUNCVAL(devapi_add_callback)},
        {LSTRKEY("close"), LFUNCVAL(devapi_server_close)},
        {LSTRKEY("__index"), LROVAL(devapiserver_map)},
        {LNILKEY, LNILVAL}
};

int load_devapi_module(lua_State *L) {
    luaL_rometatable(L, METATABLE_DEVAPI, (void *) devapiserver_map);
    return 0;
}

NODEMCU_MODULE(DEVAPI, "devapi", devapi_map, load_devapi_module);