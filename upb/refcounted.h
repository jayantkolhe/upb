/*
 * upb - a minimalist implementation of protocol buffers.
 *
 * Copyright (c) 2009-2012 Google Inc.  See LICENSE for details.
 * Author: Josh Haberman <jhaberman@gmail.com>
 *
 * A refcounting scheme that supports circular refs.  It accomplishes this by
 * partitioning the set of objects into groups such that no cycle spans groups;
 * we can then reference-count the group as a whole and ignore refs within the
 * group.  When objects are mutable, these groups are computed very
 * conservatively; we group any objects that have ever had a link between them.
 * When objects are frozen, we compute strongly-connected components which
 * allows us to be precise and only group objects that are actually cyclic.
 *
 * This is a mixed C/C++ interface that offers a full API to both languages.
 * See the top-level README for more information.
 */

#ifndef UPB_REFCOUNTED_H_
#define UPB_REFCOUNTED_H_

#include "upb/table.int.h"

// Reference tracking will check ref()/unref() operations to make sure the
// ref ownership is correct.  Where possible it will also make tools like
// Valgrind attribute ref leaks to the code that took the leaked ref, not
// the code that originally created the object.
//
// Enabling this requires the application to define upb_lock()/upb_unlock()
// functions that acquire/release a global mutex (or #define UPB_THREAD_UNSAFE).
// For this reason we don't enable it by default, even in debug builds.

// #define UPB_DEBUG_REFS

#ifdef __cplusplus
namespace upb { class RefCounted; }
#endif

UPB_DECLARE_TYPE(upb::RefCounted, upb_refcounted);

struct upb_refcounted_vtbl;

UPB_DEFINE_CLASS0(upb::RefCounted,
 public:
  // Returns true if the given object is frozen.
  bool IsFrozen() const;

  // Increases the ref count, the new ref is owned by "owner" which must not
  // already own a ref (and should not itself be a refcounted object if the ref
  // could possibly be circular; see below).
  // Thread-safe iff "this" is frozen.
  void Ref(const void *owner) const;

  // Release a ref that was acquired from upb_refcounted_ref() and collects any
  // objects it can.
  void Unref(const void *owner) const;

  // Moves an existing ref from "from" to "to", without changing the overall
  // ref count.  DonateRef(foo, NULL, owner) is the same as Ref(foo, owner),
  // but "to" may not be NULL.
  void DonateRef(const void *from, const void *to) const;

  // Verifies that a ref to the given object is currently held by the given
  // owner.  Only effective in UPB_DEBUG_REFS builds.
  void CheckRef(const void *owner) const;

 private:
  UPB_DISALLOW_POD_OPS(RefCounted, upb::RefCounted);
,
UPB_DEFINE_STRUCT0(upb_refcounted,
  // A single reference count shared by all objects in the group.
  uint32_t *group;

  // A singly-linked list of all objects in the group.
  upb_refcounted *next;

  // Table of function pointers for this type.
  const struct upb_refcounted_vtbl *vtbl;

  // Maintained only when mutable, this tracks the number of refs (but not
  // ref2's) to this object.  *group should be the sum of all individual_count
  // in the group.
  uint32_t individual_count;

  bool is_frozen;

#ifdef UPB_DEBUG_REFS
  upb_inttable *refs;  // Maps owner -> trackedref for incoming refs.
  upb_inttable *ref2s; // Set of targets for outgoing ref2s.
#endif
));

UPB_BEGIN_EXTERN_C  // {

// It is better to use tracked refs when possible, for the extra debugging
// capability.  But if this is not possible (because you don't have easy access
// to a stable pointer value that is associated with the ref), you can pass
// UPB_UNTRACKED_REF instead.
extern const void *UPB_UNTRACKED_REF;

// Native C API.
bool upb_refcounted_isfrozen(const upb_refcounted *r);
void upb_refcounted_ref(const upb_refcounted *r, const void *owner);
void upb_refcounted_unref(const upb_refcounted *r, const void *owner);
void upb_refcounted_donateref(
    const upb_refcounted *r, const void *from, const void *to);
void upb_refcounted_checkref(const upb_refcounted *r, const void *owner);


// Internal-to-upb Interface ///////////////////////////////////////////////////

typedef void upb_refcounted_visit(const upb_refcounted *r,
                                  const upb_refcounted *subobj,
                                  void *closure);

struct upb_refcounted_vtbl {
  // Must visit all subobjects that are currently ref'd via upb_refcounted_ref2.
  // Must be longjmp()-safe.
  void (*visit)(const upb_refcounted *r, upb_refcounted_visit *visit, void *c);

  // Must free the object and release all references to other objects.
  void (*free)(upb_refcounted *r);
};

// Initializes the refcounted with a single ref for the given owner.  Returns
// false if memory could not be allocated.
bool upb_refcounted_init(upb_refcounted *r,
                         const struct upb_refcounted_vtbl *vtbl,
                         const void *owner);

// Adds a ref from one refcounted object to another ("from" must not already
// own a ref).  These refs may be circular; cycles will be collected correctly
// (if conservatively).  These refs do not need to be freed in from's free()
// function.
void upb_refcounted_ref2(const upb_refcounted *r, upb_refcounted *from);

// Removes a ref that was acquired from upb_refcounted_ref2(), and collects any
// object it can.  This is only necessary when "from" no longer points to "r",
// and not from from's "free" function.
void upb_refcounted_unref2(const upb_refcounted *r, upb_refcounted *from);

#define upb_ref2(r, from) \
    upb_refcounted_ref2((const upb_refcounted*)r, (upb_refcounted*)from)
#define upb_unref2(r, from) \
    upb_refcounted_unref2((const upb_refcounted*)r, (upb_refcounted*)from)

// Freezes all mutable object reachable by ref2() refs from the given roots.
// This will split refcounting groups into precise SCC groups, so that
// refcounting of frozen objects can be more aggressive.  If memory allocation
// fails, or if more than 2**31 mutable objects are reachable from "roots", or
// if the maximum depth of the graph exceeds "maxdepth", false is returned and
// the objects are unchanged.
//
// After this operation succeeds, the objects are frozen/const, and may not be
// used through non-const pointers.  In particular, they may not be passed as
// the second parameter of upb_refcounted_{ref,unref}2().  On the upside, all
// operations on frozen refcounteds are threadsafe, and objects will be freed
// at the precise moment that they become unreachable.
//
// Caller must own refs on each object in the "roots" list.
bool upb_refcounted_freeze(upb_refcounted *const*roots, int n, upb_status *s,
                           int maxdepth);

// Shared by all compiled-in refcounted objects.
extern uint32_t static_refcount;

UPB_END_EXTERN_C  // }

#ifdef UPB_DEBUG_REFS
#define UPB_REFCOUNT_INIT(refs, ref2s) \
    {&static_refcount, NULL, NULL, 0, true, refs, ref2s}
#else
#define UPB_REFCOUNT_INIT(refs, ref2s) {&static_refcount, NULL, NULL, 0, true}
#endif

#ifdef __cplusplus
// C++ Wrappers.
namespace upb {
inline bool RefCounted::IsFrozen() const {
  return upb_refcounted_isfrozen(this);
}
inline void RefCounted::Ref(const void *owner) const {
  upb_refcounted_ref(this, owner);
}
inline void RefCounted::Unref(const void *owner) const {
  upb_refcounted_unref(this, owner);
}
inline void RefCounted::DonateRef(const void *from, const void *to) const {
  upb_refcounted_donateref(this, from, to);
}
inline void RefCounted::CheckRef(const void *owner) const {
  upb_refcounted_checkref(this, owner);
}
}  // namespace upb
#endif

#endif  // UPB_REFCOUNT_H_
