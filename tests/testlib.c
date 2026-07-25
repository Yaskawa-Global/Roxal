#include <stdbool.h>
#include <stdint.h>
float addfloats(float x, float y) { return x + y; }
bool mydoitfunction(float x, float y) { return x > y; }

typedef struct { int i; float f; } MyStruct;
float struct_sum(MyStruct s) { return s.i + s.f; }
float struct_sum_ptr(MyStruct* s) { return s->i + s->f; }
MyStruct make_struct(int i, float f) { MyStruct s; s.i = i; s.f = f; return s; }
void struct_inc(MyStruct* s) { s->i += 1; s->f += 1.0f; }

int32_t add_int32(int32_t a, int32_t b) { return a + b; }
int32_t add_uint32(uint32_t a, uint32_t b) { return (int32_t)(a + b); }
int32_t add_int16(int16_t a, int16_t b) { return (int32_t)(a + b); }
int32_t add_uint16(uint16_t a, uint16_t b) { return (int32_t)(a + b); }
uint8_t add_uint8(uint8_t a, uint8_t b) { return (uint8_t)(a + b); }
uint8_t add_int8(int8_t a, int8_t b) { return (uint8_t)(a + b); }

#include <string.h>
int cstrlen(const char* s) { return (int)strlen(s); }
void to_upper(char* s) { for (; *s; ++s) if (*s >= 'a' && *s <= 'z') *s = *s - ('a'-'A'); }

typedef struct { int i; } IntHolder;
typedef struct { double r; IntHolder h; } MyStruct2;
bool mod_nested(MyStruct2* ms2) { ms2->r = 2.22; ms2->h.i = 33; return true; }

typedef struct { IntHolder* hp; } MyStruct3;
bool mod_nested_ptr(MyStruct3* ms3) { if (ms3->hp) { ms3->hp->i += 5; return true; } return false; }

typedef struct { void* p; } VoidPtrStruct;
static int global_val = 123;
VoidPtrStruct make_voidptr_struct() { VoidPtrStruct s; s.p = &global_val; return s; }

typedef struct { double darr[4]; } ACStruct;
double sum_acstruct(ACStruct* s) { double total = 0; for(int i=0;i<4;i++) total += s->darr[i]; return total; }
void fill_acstruct(ACStruct* s) { for(int i=0;i<4;i++) s->darr[i] = (double)(i+1); }
ACStruct make_acstruct() { ACStruct s; for(int i=0;i<4;i++) s.darr[i] = (double)(i+1)*2; return s; }

double nested_sum(MyStruct2 ms2) { return ms2.r + ms2.h.i; }
MyStruct2 make_nested_struct() { MyStruct2 s; s.r = 4.5; s.h.i = 2; return s; }

typedef struct { int32_t* p; } IntBox;
void inc_intbox(IntBox* b) { if(b && b->p) (*b->p)++; }

typedef struct { uint8_t* p; } ByteBox;
void inc_bytebox(ByteBox* b) { if(b && b->p) (*b->p)++; }

#include <stdlib.h>

/* 64-bit scalars */
int64_t add_int64(int64_t a, int64_t b) { return a + b; }
uint64_t mul_uint64(uint64_t a, uint64_t b) { return a * b; }
size_t cstrlen_sz(const char* s) { return strlen(s); }

/* pointer returns, opaque handles, finalizers */
const char* greet(void) { return "hello from C"; }

typedef struct { int value; } Counter;
static int counters_freed = 0;
void* counter_new(int start) { Counter* c = (Counter*)malloc(sizeof(Counter)); c->value = start; return c; }
int counter_next(void* c) { return ((Counter*)c)->value++; }
void counter_free(void* c) { free(c); counters_freed++; }
int counters_freed_count(void) { return counters_freed; }
int is_null(const void* p) { return p == NULL; }

/* tensor/buffer passing */
void fill_ramp_u8(uint8_t* buf, int n) { for (int i = 0; i < n; i++) buf[i] = (uint8_t)(i * 10); }
float sum_f32(const float* buf, int n) { float s = 0; for (int i = 0; i < n; i++) s += buf[i]; return s; }
void scale_f64(double* buf, int n, double k) { for (int i = 0; i < n; i++) buf[i] *= k; }

int sum_or_neg(const int32_t* p, int n) { if (!p) return -1; int s = 0; for (int i = 0; i < n; i++) s += p[i]; return s; }

#include <unistd.h>
int slow_add(int a, int b, int ms) { usleep(ms * 1000); return a + b; }
void slow_fill(uint8_t* buf, int n, int ms) {
    for (int i = 0; i < n; i++) { buf[i] = (uint8_t)(i * 3); usleep(ms * 1000 / n); }
}

/* pointer-to-pointer out-parameter (the f(..., Thing** out) / Error** convention) */
static int handle_target = 99;
void make_handle(void** out) { *out = &handle_target; }
