#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

namespace roxal {

class ModuleMedia : public BuiltinModule {
public:
    ModuleMedia();
    virtual ~ModuleMedia();

    void registerBuiltins(VM& vm) override;

    inline Value moduleType() const override { return moduleTypeValue; }

private:
    Value moduleTypeValue; // ObjModuleType*

    // Helper: extract tensor from Image receiver (args[0])
    static ObjTensor* getImageTensor(ArgsView args, const char* methodName);

    // Image methods (args[0] = receiver Image instance)
    Value image_init_builtin(ArgsView args);
    Value image_write_builtin(ArgsView args);
    Value image_width_builtin(ArgsView args);
    Value image_height_builtin(ArgsView args);
    Value image_channels_builtin(ArgsView args);
    Value image_resize_builtin(ArgsView args);
    Value image_crop_builtin(ArgsView args);
    Value image_flip_horizontal_builtin(ArgsView args);
    Value image_flip_vertical_builtin(ArgsView args);
    Value image_rotate90_builtin(ArgsView args);
    Value image_rotate180_builtin(ArgsView args);
    Value image_rotate270_builtin(ArgsView args);
    Value image_grayscale_builtin(ArgsView args);
    Value image_brightness_builtin(ArgsView args);
    Value image_contrast_builtin(ArgsView args);
    Value image_saturation_builtin(ArgsView args);
    Value image_to_float_builtin(ArgsView args);
    Value image_to_uint8_builtin(ArgsView args);
};

} // namespace roxal
