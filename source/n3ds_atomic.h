#pragma once
/* Single-threaded guest only. These operations deliberately do not use host
 * atomics: x86 allows unaligned operands and ARMv6 may require libatomic for
 * 64-bit CAS. Never enable a second guest thread with this implementation. */
#include <stdint.h>
#include <string.h>
#ifdef N3DS_WATCH_STORE_CHECKS
void guest_watch_check_host(const void *p, size_t n, const char *source);
#define N3DS_WATCH_ATOMIC(p,n,s) guest_watch_check_host((p),(n),(s))
#else
#define N3DS_WATCH_ATOMIC(p,n,s) ((void)0)
#endif
#define N3DS_PTR(T,a) ({ typedef T __attribute__((aligned(1),may_alias)) _n3ds_unaligned; \
    (_n3ds_unaligned *)n3ds_guest_ptr((uint32_t)(a),sizeof(T)); })
#define N3DS_LOAD(p,order) ({ __auto_type _p=(p); __typeof__(*_p) _v; memcpy(&_v,_p,sizeof _v); _v; })
#define N3DS_EXCHANGE(p,v,order) ({ __auto_type _p=(p); __typeof__(*_p) _old,_new=(v); memcpy(&_old,_p,sizeof _old); memcpy(_p,&_new,sizeof _new); \
    N3DS_WATCH_ATOMIC(_p,sizeof *_p,"atomic-exchange"); _old; })
#define N3DS_FETCH_OP(p,v,op) ({ __auto_type _p=(p); __typeof__(*_p) _old,_new; memcpy(&_old,_p,sizeof _old); _new=_old op (v); memcpy(_p,&_new,sizeof _new); \
    N3DS_WATCH_ATOMIC(_p,sizeof *_p,"atomic-fetch"); _old; })
#define N3DS_FETCH_ADD(p,v,order) N3DS_FETCH_OP(p,v,+)
#define N3DS_FETCH_OR(p,v,order) N3DS_FETCH_OP(p,v,|)
#define N3DS_FETCH_AND(p,v,order) N3DS_FETCH_OP(p,v,&)
#define N3DS_FETCH_XOR(p,v,order) N3DS_FETCH_OP(p,v,^)
#define N3DS_CAS(p,expected,desired,weak,success,failure) ({ \
    __auto_type _p=(p); __auto_type _e=(expected); __typeof__(*_p) _old,_new=(desired); \
    memcpy(&_old,_p,sizeof _old); int _ok=(_old==*_e); \
    if(_ok) { memcpy(_p,&_new,sizeof _new); N3DS_WATCH_ATOMIC(_p,sizeof *_p,"atomic-cas"); } \
    else *_e=_old; _ok; })
