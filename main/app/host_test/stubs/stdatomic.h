#pragma once
// Single-threaded host tests: plain variables.
#define atomic_int int
#define atomic_uint unsigned
#define atomic_bool bool
#define atomic_load(p) (*(p))
#define atomic_store(p, v) (*(p) = (v))
#define atomic_fetch_add(p, v) ((*(p) += (v)) - (v))
static inline int host_xchg(int *p, int v)
{
    int o = *p;
    *p = v;
    return o;
}
#define atomic_exchange(p, v) host_xchg((int *)(p), (int)(v))
