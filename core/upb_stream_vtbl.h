/*
 * upb - a minimalist implementation of protocol buffers.
 *
 * vtable declarations for types that are implementing any of the src or sink
 * interfaces.  Only components that are implementing these interfaces need
 * to worry about this file.
 *
 * Copyright (c) 2010 Joshua Haberman.  See LICENSE for details.
 */

#ifndef UPB_SRCSINK_VTBL_H_
#define UPB_SRCSINK_VTBL_H_

#include <assert.h>
#include "upb_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

// Typedefs for function pointers to all of the virtual functions.

// upb_src
struct _upb_src {
};
typedef struct {
} upb_src_vtbl;

// upb_bytesrc.
typedef upb_strlen_t (*upb_bytesrc_read_fptr)(
    upb_bytesrc *src, void *buf, upb_strlen_t count);
typedef bool (*upb_bytesrc_getstr_fptr)(
    upb_bytesrc *src, upb_string *str, upb_strlen_t count);

// upb_bytesink.
typedef upb_strlen_t (*upb_bytesink_write_fptr)(
    upb_bytesink *bytesink, void *buf, upb_strlen_t count);
typedef upb_strlen_t (*upb_bytesink_putstr_fptr)(
    upb_bytesink *bytesink, upb_string *str);

// Vtables for the above interfaces.
typedef struct {
  upb_bytesrc_read_fptr   read;
  upb_bytesrc_getstr_fptr getstr;
} upb_bytesrc_vtable;

typedef struct {
  upb_bytesink_write_fptr  write;
  upb_bytesink_putstr_fptr putstr;
} upb_bytesink_vtable;

// "Base Class" definitions; components that implement these interfaces should
// contain one of these structures.

struct _upb_bytesrc {
  upb_bytesrc_vtable *vtbl;
  upb_status status;
  bool eof;
};

struct _upb_bytesink {
  upb_bytesink_vtable *vtbl;
  upb_status status;
  bool eof;
};

INLINE void upb_bytesrc_init(upb_bytesrc *s, upb_bytesrc_vtable *vtbl) {
  s->vtbl = vtbl;
  s->eof = false;
  upb_status_init(&s->status);
}

INLINE void upb_bytesink_init(upb_bytesink *s, upb_bytesink_vtable *vtbl) {
  s->vtbl = vtbl;
  s->eof = false;
  upb_status_init(&s->status);
}

// Implementation of virtual function dispatch.

// upb_bytesrc
INLINE upb_strlen_t upb_bytesrc_read(upb_bytesrc *src, void *buf,
                                     upb_strlen_t count) {
  return src->vtbl->read(src, buf, count);
}

INLINE bool upb_bytesrc_getstr(upb_bytesrc *src, upb_string *str,
                               upb_strlen_t count) {
  return src->vtbl->getstr(src, str, count);
}

INLINE bool upb_bytesrc_getfullstr(upb_bytesrc *src, upb_string *str,
                                   upb_status *status) {
  // We start with a getstr, because that could possibly alias data instead of
  // copying.
  if (!upb_bytesrc_getstr(src, str, UPB_STRLEN_MAX)) goto error;
  // Trade-off between number of read calls and amount of overallocation.
  const size_t bufsize = 4096;
  while (!upb_bytesrc_eof(src)) {
    upb_strlen_t len = upb_string_len(str);
    char *buf = upb_string_getrwbuf(str, len + bufsize);
    upb_strlen_t read = upb_bytesrc_read(src, buf + len, bufsize);
    if (read < 0) goto error;
    // Resize to proper size.
    upb_string_getrwbuf(str, len + read);
  }
  return true;

error:
  upb_copyerr(status, upb_bytesrc_status(src));
  return false;
}

INLINE upb_status *upb_bytesrc_status(upb_bytesrc *src) { return &src->status; }
INLINE bool upb_bytesrc_eof(upb_bytesrc *src) { return src->eof; }


// upb_bytesink
INLINE upb_strlen_t upb_bytesink_write(upb_bytesink *sink, void *buf,
                                       upb_strlen_t count) {
  return sink->vtbl->write(sink, buf, count);
}

INLINE upb_strlen_t upb_bytesink_putstr(upb_bytesink *sink, upb_string *str) {
  return sink->vtbl->putstr(sink, str);
}

INLINE upb_status *upb_bytesink_status(upb_bytesink *sink) {
  return &sink->status;
}

// upb_handlers
struct _upb_handlers {
  upb_handlerset *set;
  void *closure;
};

INLINE void upb_handlers_init(upb_handlers *h) {
  (void)h;
}
INLINE void upb_handlers_uninit(upb_handlers *h) {
  (void)h;
}

INLINE void upb_handlers_reset(upb_handlers *h) {
  h->set = NULL;
  h->closure = NULL;
}

INLINE bool upb_handlers_isempty(upb_handlers *h) {
  return !h->set && !h->closure;
}

INLINE void upb_register_handlerset(upb_handlers *h, upb_handlerset *set) {
  h->set = set;
}

INLINE void upb_set_handler_closure(upb_handlers *h, void *closure) {
  h->closure = closure;
}

// upb_dispatcher
typedef struct {
  upb_handlers handlers;
  int depth;
} upb_dispatcher_frame;

struct _upb_dispatcher {
  upb_dispatcher_frame stack[UPB_MAX_NESTING], *top, *limit;
};

INLINE void upb_dispatcher_init(upb_dispatcher *d) {
  d->limit = d->stack + sizeof(d->stack);
}

INLINE void upb_dispatcher_reset(upb_dispatcher *d, upb_handlers *h) {
  d->top = d->stack;
  d->top->depth = 1;  // Never want to trigger end-of-delegation.
  d->top->handlers = *h;
}

INLINE void upb_dispatch_startmsg(upb_dispatcher *d) {
  assert(d->stack == d->top);
  d->top->handlers.set->startmsg(d->top->handlers.closure);
}

INLINE void upb_dispatch_endmsg(upb_dispatcher *d) {
  assert(d->stack == d->top);
  d->top->handlers.set->endmsg(d->top->handlers.closure);
}

INLINE upb_flow_t upb_dispatch_startsubmsg(upb_dispatcher *d,
                                           struct _upb_fielddef *f) {
  upb_handlers handlers;
  upb_handlers_init(&handlers);
  upb_handlers_reset(&handlers);
  upb_flow_t ret = d->top->handlers.set->startsubmsg(d->top->handlers.closure, f, &handlers);
  assert((ret == UPB_DELEGATE) == !upb_handlers_isempty(&handlers));
  if (ret == UPB_DELEGATE) {
    ++d->top;
    d->top->handlers = handlers;
    d->top->depth = 0;
    d->top->handlers.set->startmsg(d->top->handlers.closure);
    ret = UPB_CONTINUE;
  }
  ++d->top->depth;
  upb_handlers_uninit(&handlers);
  return ret;
}

INLINE upb_flow_t upb_dispatch_endsubmsg(upb_dispatcher *d) {
  if (--d->top->depth == 0) {
    d->top->handlers.set->endmsg(d->top->handlers.closure);
    --d->top;
  }
  return d->top->handlers.set->endsubmsg(d->top->handlers.closure);
}

INLINE upb_flow_t upb_dispatch_value(upb_dispatcher *d,
                                     struct _upb_fielddef *f,
                                     upb_value val) {
  return d->top->handlers.set->value(d->top->handlers.closure, f, val);
}

INLINE upb_flow_t upb_dispatch_unknownval(upb_dispatcher *d,
                                          upb_field_number_t fieldnum,
                                          upb_value val) {
  return d->top->handlers.set->unknownval(d->top->handlers.closure,
                                          fieldnum, val);
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif
