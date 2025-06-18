#include <stdbool.h>
float addfloats(float x, float y) { return x + y; }
bool mydoitfunction(float x, float y) { return x > y; }

typedef struct { int i; float f; } MyStruct;
float struct_sum(MyStruct s) { return s.i + s.f; }
float struct_sum_ptr(MyStruct* s) { return s->i + s->f; }
