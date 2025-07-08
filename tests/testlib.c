#include <stdbool.h>
float addfloats(float x, float y) { return x + y; }
bool mydoitfunction(float x, float y) { return x > y; }

typedef struct { int i; float f; } MyStruct;
float struct_sum(MyStruct s) { return s.i + s.f; }
float struct_sum_ptr(MyStruct* s) { return s->i + s->f; }
MyStruct make_struct(int i, float f) { MyStruct s; s.i = i; s.f = f; return s; }
void struct_inc(MyStruct* s) { s->i += 1; s->f += 1.0f; }
