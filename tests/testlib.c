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
