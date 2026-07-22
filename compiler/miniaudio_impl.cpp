// The single translation unit compiling the vendored miniaudio implementation
// (miniaudio.h is ~90k lines; keep its compile cost out of
// ModuleMediaAudio.cpp). Runtime linking is miniaudio's default: device
// backend libraries (libasound/libpulse/...) are dlopen'd at device init, so
// this adds no link-time or install-time audio dependency.

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
