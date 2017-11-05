#include "misc/dyn_buf.h"

bool dynbuf_append(dynbuf_t* dynbuf, const char *data, size_t len) {
    if (dynbuf->data) {
        char *new_buf = c_realloc(dynbuf->data, dynbuf->length + len);
        if(new_buf == NULL){
            /**/DYNBUF_ERR("realloc FAIL! req:%u free:%u", (dynbuf->length + len), system_get_free_heap_size());
            return false;
        }
        c_memcpy((char *) new_buf + dynbuf->length, data, len);
        dynbuf->data = new_buf;
        dynbuf->length += len;
    } else {
        char *new_buf = c_malloc(len);
        if(new_buf == NULL){
            /**/DYNBUF_ERR("malloc FAIL! req:%u free:%u", (len), system_get_free_heap_size());
            return false;
        }
        c_memcpy(new_buf, data, len);
        dynbuf->data = new_buf;
        dynbuf->length = len;
    }
    return true;
}

bool dynbuf_prepend(dynbuf_t* dynbuf, const char *data, size_t len) {
    if (dynbuf->data) {
        char *new_buf = c_malloc(dynbuf->length + len);
        if(new_buf == NULL){
            /**/DYNBUF_ERR("malloc FAIL! req:%u free:%u", (dynbuf->length + len), system_get_free_heap_size());
            return false;
        }
        c_memcpy(new_buf, data, len);
        c_memcpy((char *) new_buf + len, dynbuf->data, dynbuf->length);
        c_free(dynbuf->data);
        dynbuf->data = new_buf;
        dynbuf->length += len;
    } else {
        char *new_buf = c_malloc(len);
        if(new_buf == NULL){
            /**/DYNBUF_ERR("malloc FAIL! req:%u free:%u", (len), system_get_free_heap_size());
            return false;
        }
        c_memcpy(new_buf, data, len);
        dynbuf->data = new_buf;
        dynbuf->length = len;
    }
    return true;
}

bool dynbuf_append_str(dynbuf_t* dynbuf, const char* data) {
    dynbuf_append(dynbuf, data, (size_t) c_strlen(data));
}

bool dynbuf_remove_first(dynbuf_t* dynbuf, size_t len) {
    if (dynbuf->data) {
        if (len >= dynbuf->length) {
            c_free(dynbuf->data);
            dynbuf->data = NULL;
            dynbuf->length = 0;
        } else {
            size_t new_len = dynbuf->length - len;
            char *new_buf = c_malloc(new_len);
            if(new_buf == NULL){
                /**/DYNBUF_ERR("malloc FAIL! req:%u free:%u", (new_len), system_get_free_heap_size());
                return false;
            }
            c_memcpy(new_buf, (char *) dynbuf->data + len, dynbuf->length - len);
            c_free(dynbuf->data);
            dynbuf->data = new_buf;
            dynbuf->length = new_len;
        }
    }
    return true;
}

bool dynbuf_replace(dynbuf_t* dynbuf, unsigned int offset, size_t oryg_len, const char* data, size_t len) {
    size_t new_len = dynbuf->length + len - oryg_len;
    if (new_len > dynbuf->length) {
        char *new_buf = c_realloc(dynbuf->data, new_len);
        if(new_buf == NULL){
            /**/DYNBUF_ERR("realloc FAIL! req:%u free:%u", (dynbuf->length + len), system_get_free_heap_size());
            return false;
        }
        dynbuf->data = new_buf;
        os_memmove(dynbuf->data + offset + len, dynbuf->data + offset + oryg_len, dynbuf->length - offset - oryg_len);
        dynbuf->length = new_len;
    } else if (new_len < dynbuf->length){
        os_memmove(dynbuf->data + offset + len, dynbuf->data + offset + oryg_len, dynbuf->length - offset - oryg_len);
        char *new_buf = c_realloc(dynbuf->data, new_len);
        if(new_buf == NULL){
            /**/DYNBUF_ERR("realloc FAIL! req:%u free:%u", (dynbuf->length + len), system_get_free_heap_size());
            return false;
        }
        dynbuf->data = new_buf;
        dynbuf->length = new_len;
    }
    c_memcpy(dynbuf->data + offset, data, len);
}

void dynbuf_free(dynbuf_t* dynbuf) {
    if (dynbuf->data) {
        c_free(dynbuf->data);
        dynbuf->data = NULL;
    }
    dynbuf->length = 0;
}
