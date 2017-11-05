#ifndef __DYNBUF_H__
#define __DYNBUF_H__
#include "user_interface.h"
#include "c_stdio.h"
#include "c_stdlib.h"
#include "c_string.h"

//#define DYNBUF_ERROR

typedef struct _dynbuf{
  char* data;
  size_t length;
} dynbuf_t;

bool dynbuf_append(dynbuf_t* dynbuf, const char* data, size_t len);
bool dynbuf_prepend(dynbuf_t* dynbuf, const char* data, size_t len);

bool dynbuf_append_str(dynbuf_t* dynbuf, const char* data);
bool dynbuf_prepend_str(dynbuf_t* dynbuf, const char* data);

bool dynbuf_remove_first(dynbuf_t* dynbuf, size_t len);
bool dynbuf_replace(dynbuf_t* dynbuf, unsigned int offset, size_t oryg_len, const char* data, size_t len);

void dynbuf_free(dynbuf_t* dynbuf);

#if 0 || defined(DYNBUF_ERROR) || defined(NODE_ERROR)
#define DYNBUF_ERR(fmt, ...) c_printf("\n DYNBUF: "fmt"\n", ##__VA_ARGS__)
#else
#define DYNBUF_ERR(...)
#endif

#endif // __DYNBUF_H__
