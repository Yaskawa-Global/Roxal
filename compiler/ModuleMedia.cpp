#include "ModuleMedia.h"
#include "VM.h"
#include "Object.h"
#include <stdexcept>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include <png.h>
#include <jpeglib.h>

using namespace roxal;

// ============================================================
// Helpers
// ============================================================

static std::string toLower(const std::string& s)
{
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

// Returns true when rawData()/rawDataMut() yields a properly-typed buffer
// (i.e. uint8 bytes for UInt8 dtype). When false, the backing store is
// std::vector<double> and we must use at()/setAt() instead.
static bool hasTypedStorage(const ObjTensor* t)
{
    return t->isOrtBacked();
}

ObjTensor* ModuleMedia::getImageTensor(ArgsView args, const char* methodName)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument(std::string("Image.") + methodName + " expects receiver");
    ObjectInstance* inst = asObjectInstance(args[0]);
    Value dataVal = inst->getProperty("data");
    if (!isTensor(dataVal))
        throw std::runtime_error(std::string("Image.") + methodName + ": image has no tensor data");
    return asTensor(dataVal);
}

// Copy an H*W*C uint8 buffer into a tensor.  Uses bulk memcpy when ORT-backed,
// otherwise falls back to setAt() which stores values as doubles.
static void copyBytesToTensor(ObjTensor* dst, const uint8_t* src, int64_t count)
{
    if (hasTypedStorage(dst)) {
        std::memcpy(dst->rawDataMut(), src, count);
    } else {
        for (int64_t i = 0; i < count; ++i)
            dst->setAt(i, static_cast<double>(src[i]));
    }
}

// Copy tensor data into an H*W*C uint8 buffer.
static void copyTensorToBytes(const ObjTensor* src, uint8_t* dst, int64_t count)
{
    if (hasTypedStorage(src)) {
        std::memcpy(dst, src->rawData(), count);
    } else {
        for (int64_t i = 0; i < count; ++i)
            dst[i] = static_cast<uint8_t>(std::clamp(std::round(src->at(i)), 0.0, 255.0));
    }
}

// Read a single uint8 element from a tensor (regardless of storage type)
static inline uint8_t tensorGetU8(const ObjTensor* t, const uint8_t* typed, int64_t i)
{
    if (typed) return typed[i];
    return static_cast<uint8_t>(std::clamp(std::round(t->at(i)), 0.0, 255.0));
}

// ============================================================
// PNG I/O
// ============================================================

static Value readPNG(const std::string& path, int requestedChannels)
{
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp)
        throw std::runtime_error("media.Image: cannot open '" + path + "'");

    png_byte header[8];
    if (fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8)) {
        fclose(fp);
        throw std::runtime_error("media.Image: '" + path + "' is not a valid PNG file");
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { fclose(fp); throw std::runtime_error("media.Image: png_create_read_struct failed"); }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, nullptr, nullptr); fclose(fp); throw std::runtime_error("media.Image: png_create_info_struct failed"); }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(fp);
        throw std::runtime_error("media.Image: error reading PNG '" + path + "'");
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    int width = png_get_image_width(png, info);
    int height = png_get_image_height(png, info);
    png_byte colorType = png_get_color_type(png, info);
    png_byte bitDepth = png_get_bit_depth(png, info);

    // Apply transforms to get 8-bit per channel data
    if (bitDepth == 16)
        png_set_strip_16(png);
    if (colorType == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);

    // Handle requested channel conversion
    if (requestedChannels == 1) {
        if (colorType == PNG_COLOR_TYPE_RGB || colorType == PNG_COLOR_TYPE_RGB_ALPHA ||
            colorType == PNG_COLOR_TYPE_PALETTE)
            png_set_rgb_to_gray_fixed(png, 1, -1, -1);
        if (colorType == PNG_COLOR_TYPE_GRAY_ALPHA || colorType == PNG_COLOR_TYPE_RGB_ALPHA)
            png_set_strip_alpha(png);
    } else if (requestedChannels == 3) {
        if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
            png_set_gray_to_rgb(png);
        if (colorType == PNG_COLOR_TYPE_GRAY_ALPHA || colorType == PNG_COLOR_TYPE_RGB_ALPHA)
            png_set_strip_alpha(png);
    } else if (requestedChannels == 4) {
        if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
            png_set_gray_to_rgb(png);
        if (!(colorType & PNG_COLOR_MASK_ALPHA))
            png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    }

    png_read_update_info(png, info);

    int channels = png_get_channels(png, info);
    size_t rowBytes = png_get_rowbytes(png, info);

    // Read into temporary buffer first
    std::vector<uint8_t> imgBuf(height * rowBytes);
    std::vector<png_bytep> rowPtrs(height);
    for (int y = 0; y < height; ++y)
        rowPtrs[y] = imgBuf.data() + y * rowBytes;

    png_read_image(png, rowPtrs.data());
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(fp);

    // Create tensor and copy data in
    std::vector<int64_t> shape = {height, width, channels};
    auto tensor = newTensorObj(shape, TensorDType::UInt8);
    copyBytesToTensor(tensor.get(), imgBuf.data(), height * width * channels);

    return Value::objVal(std::move(tensor));
}

static void writePNG(const std::string& path, ObjTensor* tensor)
{
    if (tensor->rank() != 3)
        throw std::invalid_argument("Image.write: tensor must be 3D [H, W, C]");
    if (tensor->dtype() != TensorDType::UInt8)
        throw std::invalid_argument("Image.write: PNG requires uint8 tensor (use to_uint8() first)");

    int height = static_cast<int>(tensor->shape()[0]);
    int width = static_cast<int>(tensor->shape()[1]);
    int channels = static_cast<int>(tensor->shape()[2]);

    int pngColorType;
    switch (channels) {
        case 1: pngColorType = PNG_COLOR_TYPE_GRAY; break;
        case 2: pngColorType = PNG_COLOR_TYPE_GRAY_ALPHA; break;
        case 3: pngColorType = PNG_COLOR_TYPE_RGB; break;
        case 4: pngColorType = PNG_COLOR_TYPE_RGBA; break;
        default: throw std::invalid_argument("Image.write: unsupported channel count " + std::to_string(channels));
    }

    // Copy tensor data to a contiguous byte buffer
    int64_t totalBytes = static_cast<int64_t>(height) * width * channels;
    std::vector<uint8_t> imgBuf(totalBytes);
    copyTensorToBytes(tensor, imgBuf.data(), totalBytes);

    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp)
        throw std::runtime_error("Image.write: cannot create '" + path + "'");

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { fclose(fp); throw std::runtime_error("Image.write: png_create_write_struct failed"); }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); fclose(fp); throw std::runtime_error("Image.write: png_create_info_struct failed"); }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        throw std::runtime_error("Image.write: error writing PNG '" + path + "'");
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, width, height, 8, pngColorType,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    size_t rowBytes = width * channels;
    std::vector<const png_byte*> rowPtrs(height);
    for (int y = 0; y < height; ++y)
        rowPtrs[y] = imgBuf.data() + y * rowBytes;

    png_write_image(png, const_cast<png_bytepp>(rowPtrs.data()));
    png_write_end(png, nullptr);

    png_destroy_write_struct(&png, &info);
    fclose(fp);
}

// ============================================================
// JPEG I/O
// ============================================================

static Value readJPEG(const std::string& path, int requestedChannels)
{
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp)
        throw std::runtime_error("media.Image: cannot open '" + path + "'");

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);

    if (requestedChannels == 1)
        cinfo.out_color_space = JCS_GRAYSCALE;
    else
        cinfo.out_color_space = JCS_RGB;

    jpeg_start_decompress(&cinfo);

    int width = cinfo.output_width;
    int height = cinfo.output_height;
    int jpegChannels = cinfo.output_components;

    int outChannels = (requestedChannels == 4) ? 4 : jpegChannels;
    if (requestedChannels > 0 && requestedChannels != 4)
        outChannels = requestedChannels;

    // Read into temporary buffer
    std::vector<uint8_t> imgBuf(height * width * outChannels);

    if (requestedChannels == 4) {
        std::vector<uint8_t> rowBuf(width * jpegChannels);
        for (int y = 0; y < height; ++y) {
            uint8_t* rowPtr = rowBuf.data();
            jpeg_read_scanlines(&cinfo, &rowPtr, 1);
            uint8_t* dst = imgBuf.data() + y * width * 4;
            for (int x = 0; x < width; ++x) {
                dst[x * 4 + 0] = rowBuf[x * 3 + 0];
                dst[x * 4 + 1] = rowBuf[x * 3 + 1];
                dst[x * 4 + 2] = rowBuf[x * 3 + 2];
                dst[x * 4 + 3] = 255;
            }
        }
    } else {
        for (int y = 0; y < height; ++y) {
            uint8_t* rowPtr = imgBuf.data() + y * width * outChannels;
            jpeg_read_scanlines(&cinfo, &rowPtr, 1);
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);

    std::vector<int64_t> shape = {height, width, outChannels};
    auto tensor = newTensorObj(shape, TensorDType::UInt8);
    copyBytesToTensor(tensor.get(), imgBuf.data(), height * width * outChannels);

    return Value::objVal(std::move(tensor));
}

static void writeJPEG(const std::string& path, ObjTensor* tensor, int quality)
{
    if (tensor->rank() != 3)
        throw std::invalid_argument("Image.write: tensor must be 3D [H, W, C]");
    if (tensor->dtype() != TensorDType::UInt8)
        throw std::invalid_argument("Image.write: JPEG requires uint8 tensor (use to_uint8() first)");

    int height = static_cast<int>(tensor->shape()[0]);
    int width = static_cast<int>(tensor->shape()[1]);
    int channels = static_cast<int>(tensor->shape()[2]);

    if (channels == 2 || channels == 4)
        throw std::invalid_argument("Image.write: JPEG does not support alpha channel (" +
                                    std::to_string(channels) + " channels)");
    if (channels != 1 && channels != 3)
        throw std::invalid_argument("Image.write: JPEG requires 1 or 3 channels, got " +
                                    std::to_string(channels));

    // Copy tensor data to contiguous byte buffer
    int64_t totalBytes = static_cast<int64_t>(height) * width * channels;
    std::vector<uint8_t> imgBuf(totalBytes);
    copyTensorToBytes(tensor, imgBuf.data(), totalBytes);

    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp)
        throw std::runtime_error("Image.write: cannot create '" + path + "'");

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = channels;
    cinfo.in_color_space = (channels == 1) ? JCS_GRAYSCALE : JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    for (int y = 0; y < height; ++y) {
        const uint8_t* rowPtr = imgBuf.data() + y * width * channels;
        jpeg_write_scanlines(&cinfo, const_cast<JSAMPARRAY>(&rowPtr), 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
}

// ============================================================
// Module constructor/destructor/registration
// ============================================================

ModuleMedia::ModuleMedia()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("media")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleMedia::~ModuleMedia()
{
    destroyModuleType(moduleTypeValue);
}

void ModuleMedia::registerBuiltins(VM& vm)
{
    setVM(vm);

    linkMethod("Image", "init",            [this](VM&, ArgsView a) { return image_init_builtin(a); });
    linkMethod("Image", "write",           [this](VM&, ArgsView a) { return image_write_builtin(a); });
    linkMethod("Image", "width",           [this](VM&, ArgsView a) { return image_width_builtin(a); });
    linkMethod("Image", "height",          [this](VM&, ArgsView a) { return image_height_builtin(a); });
    linkMethod("Image", "channels",        [this](VM&, ArgsView a) { return image_channels_builtin(a); });
    linkMethod("Image", "resize",          [this](VM&, ArgsView a) { return image_resize_builtin(a); });
    linkMethod("Image", "crop",            [this](VM&, ArgsView a) { return image_crop_builtin(a); });
    linkMethod("Image", "flip_horizontal", [this](VM&, ArgsView a) { return image_flip_horizontal_builtin(a); });
    linkMethod("Image", "flip_vertical",   [this](VM&, ArgsView a) { return image_flip_vertical_builtin(a); });
    linkMethod("Image", "rotate90",        [this](VM&, ArgsView a) { return image_rotate90_builtin(a); });
    linkMethod("Image", "rotate180",       [this](VM&, ArgsView a) { return image_rotate180_builtin(a); });
    linkMethod("Image", "rotate270",       [this](VM&, ArgsView a) { return image_rotate270_builtin(a); });
    linkMethod("Image", "grayscale",       [this](VM&, ArgsView a) { return image_grayscale_builtin(a); });
    linkMethod("Image", "brightness",      [this](VM&, ArgsView a) { return image_brightness_builtin(a); });
    linkMethod("Image", "contrast",        [this](VM&, ArgsView a) { return image_contrast_builtin(a); });
    linkMethod("Image", "saturation",      [this](VM&, ArgsView a) { return image_saturation_builtin(a); });
    linkMethod("Image", "to_float",        [this](VM&, ArgsView a) { return image_to_float_builtin(a); });
    linkMethod("Image", "to_uint8",        [this](VM&, ArgsView a) { return image_to_uint8_builtin(a); });
}

// Helper: replace the receiver's data tensor in-place
static void setImageData(ArgsView args, Value tensorVal)
{
    asObjectInstance(args[0])->setProperty("data", tensorVal);
}

// ============================================================
// Image.init(path, tensor, channels)
// ============================================================

Value ModuleMedia::image_init_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Image.init expects receiver");

    ObjectInstance* inst = asObjectInstance(args[0]);

    // Get path argument (args[1])
    std::string path;
    if (args.size() >= 2 && isString(args[1]))
        path = toUTF8StdString(asStringObj(args[1])->s);

    // Get tensor argument (args[2])
    bool hasTensor = (args.size() >= 3 && isTensor(args[2]));

    // Get channels argument (args[3])
    int requestedChannels = 0;
    if (args.size() >= 4 && !args[3].isNil())
        requestedChannels = static_cast<int>(toType(ValueType::Int, args[3], false).asInt());

    if (!path.empty() && hasTensor)
        throw std::invalid_argument("Image.init: provide either path or tensor, not both");

    if (hasTensor) {
        ObjTensor* t = asTensor(args[2]);
        if (t->rank() != 3)
            throw std::invalid_argument("Image.init: tensor must be 3D [H, W, C], got rank " +
                                        std::to_string(t->rank()));
        inst->setProperty("data", args[2]);
        return Value::nilVal();
    }

    if (path.empty())
        throw std::invalid_argument("Image.init: provide a file path or tensor");

    std::string ext = toLower(std::filesystem::path(path).extension().string());

    Value tensorVal;
    if (ext == ".png") {
        tensorVal = readPNG(path, requestedChannels);
    } else if (ext == ".jpg" || ext == ".jpeg") {
        tensorVal = readJPEG(path, requestedChannels);
    } else {
        throw std::invalid_argument("Image.init: unsupported format '" + ext +
                                    "' (supported: .png, .jpg, .jpeg)");
    }

    inst->setProperty("data", tensorVal);
    return Value::nilVal();
}

// ============================================================
// Image.write(path, quality)
// ============================================================

Value ModuleMedia::image_write_builtin(ArgsView args)
{
    if (args.size() < 2 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Image.write expects receiver and path");
    if (!isString(args[1]))
        throw std::invalid_argument("Image.write: path must be a string");

    ObjTensor* tensor = getImageTensor(args, "write");
    std::string path = toUTF8StdString(asStringObj(args[1])->s);
    int quality = 95;
    if (args.size() >= 3 && !args[2].isNil())
        quality = static_cast<int>(toType(ValueType::Int, args[2], false).asInt());

    std::string ext = toLower(std::filesystem::path(path).extension().string());

    if (ext == ".png") {
        writePNG(path, tensor);
    } else if (ext == ".jpg" || ext == ".jpeg") {
        writeJPEG(path, tensor, quality);
    } else {
        throw std::invalid_argument("Image.write: unsupported format '" + ext +
                                    "' (supported: .png, .jpg, .jpeg)");
    }

    return Value::boolVal(true);
}

// ============================================================
// Query methods
// ============================================================

Value ModuleMedia::image_width_builtin(ArgsView args)
{
    ObjTensor* t = getImageTensor(args, "width");
    return Value::intVal(t->shape()[1]); // [H, W, C]
}

Value ModuleMedia::image_height_builtin(ArgsView args)
{
    ObjTensor* t = getImageTensor(args, "height");
    return Value::intVal(t->shape()[0]); // [H, W, C]
}

Value ModuleMedia::image_channels_builtin(ArgsView args)
{
    ObjTensor* t = getImageTensor(args, "channels");
    return Value::intVal(t->shape()[2]); // [H, W, C]
}

// ============================================================
// Resize (bilinear interpolation)
// ============================================================

Value ModuleMedia::image_resize_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "resize");
    if (args.size() < 3)
        throw std::invalid_argument("Image.resize expects width and height");

    int dstW = static_cast<int>(toType(ValueType::Int, args[1], false).asInt());
    int dstH = static_cast<int>(toType(ValueType::Int, args[2], false).asInt());
    if (dstW <= 0 || dstH <= 0)
        throw std::invalid_argument("Image.resize: width and height must be positive");

    int srcH = static_cast<int>(src->shape()[0]);
    int srcW = static_cast<int>(src->shape()[1]);
    int ch   = static_cast<int>(src->shape()[2]);
    TensorDType dtype = src->dtype();

    std::vector<int64_t> dstShape = {dstH, dstW, ch};
    auto dst = newTensorObj(dstShape, dtype);

    // Typed fast path for UInt8 with ORT storage
    const uint8_t* srcTyped = (dtype == TensorDType::UInt8 && hasTypedStorage(src))
        ? static_cast<const uint8_t*>(src->rawData()) : nullptr;

    for (int y = 0; y < dstH; ++y) {
        double srcY = (dstH > 1) ? y * (srcH - 1.0) / (dstH - 1.0) : 0.0;
        int y0 = static_cast<int>(srcY);
        int y1 = std::min(y0 + 1, srcH - 1);
        double fy = srcY - y0;

        for (int x = 0; x < dstW; ++x) {
            double srcX = (dstW > 1) ? x * (srcW - 1.0) / (dstW - 1.0) : 0.0;
            int x0 = static_cast<int>(srcX);
            int x1 = std::min(x0 + 1, srcW - 1);
            double fx = srcX - x0;

            for (int c = 0; c < ch; ++c) {
                int i00 = (y0 * srcW + x0) * ch + c;
                int i01 = (y0 * srcW + x1) * ch + c;
                int i10 = (y1 * srcW + x0) * ch + c;
                int i11 = (y1 * srcW + x1) * ch + c;

                double v00, v01, v10, v11;
                if (srcTyped) {
                    v00 = srcTyped[i00]; v01 = srcTyped[i01];
                    v10 = srcTyped[i10]; v11 = srcTyped[i11];
                } else {
                    v00 = src->at(i00); v01 = src->at(i01);
                    v10 = src->at(i10); v11 = src->at(i11);
                }

                double val = (1 - fy) * ((1 - fx) * v00 + fx * v01) +
                             fy * ((1 - fx) * v10 + fx * v11);

                int di = (y * dstW + x) * ch + c;
                if (dtype == TensorDType::UInt8) {
                    dst->setAt(di, std::clamp(std::round(val), 0.0, 255.0));
                } else {
                    dst->setAt(di, val);
                }
            }
        }
    }

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Crop
// ============================================================

Value ModuleMedia::image_crop_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "crop");
    if (args.size() < 5)
        throw std::invalid_argument("Image.crop expects x, y, width, height");

    int cx = static_cast<int>(toType(ValueType::Int, args[1], false).asInt());
    int cy = static_cast<int>(toType(ValueType::Int, args[2], false).asInt());
    int cw = static_cast<int>(toType(ValueType::Int, args[3], false).asInt());
    int cropH = static_cast<int>(toType(ValueType::Int, args[4], false).asInt());

    int srcH = static_cast<int>(src->shape()[0]);
    int srcW = static_cast<int>(src->shape()[1]);
    int channels = static_cast<int>(src->shape()[2]);

    if (cx < 0 || cy < 0 || cw <= 0 || cropH <= 0 ||
        cx + cw > srcW || cy + cropH > srcH)
        throw std::invalid_argument("Image.crop: region exceeds image bounds");

    TensorDType dtype = src->dtype();
    std::vector<int64_t> dstShape = {cropH, cw, channels};
    auto dst = newTensorObj(dstShape, dtype);

    for (int y = 0; y < cropH; ++y)
        for (int x = 0; x < cw; ++x)
            for (int c = 0; c < channels; ++c)
                dst->setAt((y * cw + x) * channels + c,
                           src->at(((cy + y) * srcW + (cx + x)) * channels + c));

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Flip horizontal
// ============================================================

Value ModuleMedia::image_flip_horizontal_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "flip_horizontal");
    int h = static_cast<int>(src->shape()[0]);
    int w = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    auto dst = newTensorObj(src->shape(), src->dtype());

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c)
                dst->setAt((y * w + (w - 1 - x)) * ch + c,
                           src->at((y * w + x) * ch + c));

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Flip vertical
// ============================================================

Value ModuleMedia::image_flip_vertical_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "flip_vertical");
    int h = static_cast<int>(src->shape()[0]);
    int w = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    auto dst = newTensorObj(src->shape(), src->dtype());

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c)
                dst->setAt(((h - 1 - y) * w + x) * ch + c,
                           src->at((y * w + x) * ch + c));

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Rotate 90 degrees clockwise
// ============================================================

Value ModuleMedia::image_rotate90_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "rotate90");
    int h = static_cast<int>(src->shape()[0]);
    int w = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    // Output: [W, H, C]
    std::vector<int64_t> dstShape = {w, h, ch};
    auto dst = newTensorObj(dstShape, src->dtype());

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c)
                dst->setAt((x * h + (h - 1 - y)) * ch + c,
                           src->at((y * w + x) * ch + c));

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Rotate 180 degrees
// ============================================================

Value ModuleMedia::image_rotate180_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "rotate180");
    int h = static_cast<int>(src->shape()[0]);
    int w = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    auto dst = newTensorObj(src->shape(), src->dtype());

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c)
                dst->setAt(((h - 1 - y) * w + (w - 1 - x)) * ch + c,
                           src->at((y * w + x) * ch + c));

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Rotate 270 degrees clockwise (= 90 counter-clockwise)
// ============================================================

Value ModuleMedia::image_rotate270_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "rotate270");
    int h = static_cast<int>(src->shape()[0]);
    int w = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    // Output: [W, H, C]
    std::vector<int64_t> dstShape = {w, h, ch};
    auto dst = newTensorObj(dstShape, src->dtype());

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c)
                dst->setAt(((w - 1 - x) * h + y) * ch + c,
                           src->at((y * w + x) * ch + c));

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Grayscale
// ============================================================

Value ModuleMedia::image_grayscale_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "grayscale");
    int h = static_cast<int>(src->shape()[0]);
    int w = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    if (ch != 3 && ch != 4)
        throw std::invalid_argument("Image.grayscale: requires RGB (3) or RGBA (4) channels, got " +
                                    std::to_string(ch));

    std::vector<int64_t> dstShape = {h, w, 1};
    auto dst = newTensorObj(dstShape, src->dtype());

    for (int i = 0; i < h * w; ++i) {
        double r = src->at(i * ch + 0);
        double g = src->at(i * ch + 1);
        double b = src->at(i * ch + 2);
        double gray = 0.299 * r + 0.587 * g + 0.114 * b;
        if (src->dtype() == TensorDType::UInt8)
            gray = std::clamp(std::round(gray), 0.0, 255.0);
        dst->setAt(i, gray);
    }

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Brightness
// ============================================================

Value ModuleMedia::image_brightness_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "brightness");
    if (args.size() < 2)
        throw std::invalid_argument("Image.brightness expects factor");
    double factor = toType(ValueType::Real, args[1], false).asReal();

    int64_t total = src->numel();
    auto dst = newTensorObj(src->shape(), src->dtype());

    double maxVal = (src->dtype() == TensorDType::UInt8) ? 255.0 : 1.0;
    for (int64_t i = 0; i < total; ++i) {
        double val = std::clamp(src->at(i) * factor, 0.0, maxVal);
        if (src->dtype() == TensorDType::UInt8)
            val = std::round(val);
        dst->setAt(i, val);
    }

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Contrast
// ============================================================

Value ModuleMedia::image_contrast_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "contrast");
    if (args.size() < 2)
        throw std::invalid_argument("Image.contrast expects factor");
    double factor = toType(ValueType::Real, args[1], false).asReal();

    int64_t total = src->numel();

    // Compute mean intensity
    double sum = 0;
    for (int64_t i = 0; i < total; ++i)
        sum += src->at(i);
    double mean = sum / total;

    double maxVal = (src->dtype() == TensorDType::UInt8) ? 255.0 : 1.0;
    auto dst = newTensorObj(src->shape(), src->dtype());

    for (int64_t i = 0; i < total; ++i) {
        double val = std::clamp(mean + factor * (src->at(i) - mean), 0.0, maxVal);
        if (src->dtype() == TensorDType::UInt8)
            val = std::round(val);
        dst->setAt(i, val);
    }

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Saturation
// ============================================================

Value ModuleMedia::image_saturation_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "saturation");
    if (args.size() < 2)
        throw std::invalid_argument("Image.saturation expects factor");
    double factor = toType(ValueType::Real, args[1], false).asReal();

    int h = static_cast<int>(src->shape()[0]);
    int w = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    if (ch < 3)
        throw std::invalid_argument("Image.saturation: requires at least 3 channels (RGB)");

    double maxVal = (src->dtype() == TensorDType::UInt8) ? 255.0 : 1.0;
    auto dst = newTensorObj(src->shape(), src->dtype());

    for (int i = 0; i < h * w; ++i) {
        double r = src->at(i * ch + 0);
        double g = src->at(i * ch + 1);
        double b = src->at(i * ch + 2);
        double gray = 0.299 * r + 0.587 * g + 0.114 * b;

        auto adjust = [&](double v) -> double {
            double val = std::clamp(gray + factor * (v - gray), 0.0, maxVal);
            return (src->dtype() == TensorDType::UInt8) ? std::round(val) : val;
        };

        dst->setAt(i * ch + 0, adjust(r));
        dst->setAt(i * ch + 1, adjust(g));
        dst->setAt(i * ch + 2, adjust(b));
        // Copy alpha if present
        if (ch == 4)
            dst->setAt(i * ch + 3, src->at(i * ch + 3));
    }

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// to_float: uint8 [0-255] -> float32 [0.0-1.0]
// ============================================================

Value ModuleMedia::image_to_float_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "to_float");
    if (src->dtype() != TensorDType::UInt8)
        throw std::invalid_argument("Image.to_float: tensor must be uint8");

    int64_t total = src->numel();
    auto dst = newTensorObj(src->shape(), TensorDType::Float32);

    for (int64_t i = 0; i < total; ++i)
        dst->setAt(i, src->at(i) / 255.0);

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// to_uint8: float32/float64 [0.0-1.0] -> uint8 [0-255]
// ============================================================

Value ModuleMedia::image_to_uint8_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "to_uint8");
    if (src->dtype() != TensorDType::Float32 && src->dtype() != TensorDType::Float64)
        throw std::invalid_argument("Image.to_uint8: tensor must be float32 or float64");

    int64_t total = src->numel();
    auto dst = newTensorObj(src->shape(), TensorDType::UInt8);

    for (int64_t i = 0; i < total; ++i) {
        double val = src->at(i) * 255.0;
        dst->setAt(i, std::clamp(std::round(val), 0.0, 255.0));
    }

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

