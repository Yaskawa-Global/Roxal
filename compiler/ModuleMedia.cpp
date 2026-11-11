#include "ModuleMedia.h"
#include "VM.h"
#include "Object.h"
#include <stdexcept>
#include <string>
#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <vector>
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

// Copy an H*W*C uint8 buffer into / out of a uint8 tensor.
static void copyBytesToTensor(ObjTensor* dst, const uint8_t* src, int64_t count)
{
    std::memcpy(dst->rawDataMut(), src, count);
}

static void copyTensorToBytes(const ObjTensor* src, uint8_t* dst, int64_t count)
{
    std::memcpy(dst, src->rawData(), count);
}

namespace {

// Element views over a tensor's dtype-native storage, taken once per builtin so
// pixel loops are plain loads and stores.  Pixel loops must not use per-element
// ObjTensor::at()/setAt(): every setAt() is a full mutation (a global write-epoch
// bump and, while any frozen snapshot is alive, an MVCC version save that copies
// the whole buffer whenever a new snapshot has been taken since the last save).
struct PixelsIn {
    explicit PixelsIn(const ObjTensor* t) : base(t->rawData()), dtype(t->dtype()) {}
    double operator[](int64_t i) const { return tensorRawElementAsDouble(base, dtype, i); }
    const void* base;
    TensorDType dtype;
};

struct PixelsOut {
    explicit PixelsOut(ObjTensor* t) : base(t->rawDataMut()), dtype(t->dtype()) {}
    void set(int64_t i, double v) const { tensorRawSetElementFromDouble(base, dtype, i, v); }
    void* base;
    TensorDType dtype;
};

}

// ============================================================
// Image codecs
// ============================================================
// PNG and JPEG are encoded to and decoded from memory; files are read or
// written whole around that.  A codec's error -- a corrupt or truncated
// image -- is raised as an exception: libpng and libjpeg report errors by
// longjmp, back into the function that set the jump, so every buffer that
// must outlive the jump is declared before it.

static std::vector<uint8_t> readFileBytes(const std::string& path, const char* what)
{
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp)
        throw std::runtime_error(std::string(what) + ": cannot open '" + path + "'");
    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, fp)) > 0)
        bytes.insert(bytes.end(), chunk, chunk + n);
    const bool failed = ferror(fp) != 0;
    fclose(fp);
    if (failed)
        throw std::runtime_error(std::string(what) + ": cannot read '" + path + "'");
    return bytes;
}

static void writeFileBytes(const std::string& path, const std::vector<uint8_t>& bytes)
{
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp)
        throw std::runtime_error("Image.write: cannot create '" + path + "'");
    const bool ok = bytes.empty() || fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size();
    const bool closed = fclose(fp) == 0;
    if (!ok || !closed)
        throw std::runtime_error("Image.write: cannot write '" + path + "'");
}

static bool isPNG(const uint8_t* data, size_t size)
{
    return size >= 8 && png_sig_cmp(const_cast<png_bytep>(data), 0, 8) == 0;
}

static bool isJPEG(const uint8_t* data, size_t size)
{
    return size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

// [H, W, C] uint8 pixels, checked for encoding.
static void encodableImage(const ObjTensor* tensor, const char* what, const char* format,
                           int& height, int& width, int& channels)
{
    if (tensor->rank() != 3)
        throw std::invalid_argument(std::string(what) + ": tensor must be 3D [H, W, C]");
    if (tensor->dtype() != TensorDType::UInt8)
        throw std::invalid_argument(std::string(what) + ": " + format +
                                    " requires uint8 tensor (use to_uint8() first)");
    height = static_cast<int>(tensor->shape()[0]);
    width = static_cast<int>(tensor->shape()[1]);
    channels = static_cast<int>(tensor->shape()[2]);
}

// ---- PNG ----

namespace {
struct PngSource {
    const uint8_t* data;
    size_t size;
    size_t at;
};
}

static void pngReadMemory(png_structp png, png_bytep out, png_size_t n)
{
    auto* src = static_cast<PngSource*>(png_get_io_ptr(png));
    if (src->size - src->at < n)
        png_error(png, "truncated");
    std::memcpy(out, src->data + src->at, n);
    src->at += n;
}

static void pngWriteMemory(png_structp png, png_bytep data, png_size_t n)
{
    auto* out = static_cast<std::vector<uint8_t>*>(png_get_io_ptr(png));
    out->insert(out->end(), data, data + n);
}

static void pngFlushMemory(png_structp) {}

static Value decodePNG(const uint8_t* data, size_t size, int requestedChannels, const std::string& what)
{
    if (!isPNG(data, size))
        throw std::runtime_error("media.Image: " + what + " is not a valid PNG image");

    PngSource source { data, size, 8 };
    std::vector<uint8_t> imgBuf;
    std::vector<png_bytep> rowPtrs;
    int width = 0, height = 0, channels = 0;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) throw std::runtime_error("media.Image: png_create_read_struct failed");
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, nullptr, nullptr); throw std::runtime_error("media.Image: png_create_info_struct failed"); }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        throw std::runtime_error("media.Image: error decoding PNG " + what);
    }

    png_set_read_fn(png, &source, &pngReadMemory);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    width = png_get_image_width(png, info);
    height = png_get_image_height(png, info);
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

    channels = png_get_channels(png, info);
    size_t rowBytes = png_get_rowbytes(png, info);

    imgBuf.resize(static_cast<size_t>(height) * rowBytes);
    rowPtrs.resize(height);
    for (int y = 0; y < height; ++y)
        rowPtrs[y] = imgBuf.data() + y * rowBytes;

    png_read_image(png, rowPtrs.data());
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);

    std::vector<int64_t> shape = {height, width, channels};
    auto tensor = newTensorObj(shape, TensorDType::UInt8);
    copyBytesToTensor(tensor.get(), imgBuf.data(), static_cast<int64_t>(height) * width * channels);
    return Value::objVal(std::move(tensor));
}

static std::vector<uint8_t> encodePNG(ObjTensor* tensor, const char* what)
{
    int height, width, channels;
    encodableImage(tensor, what, "PNG", height, width, channels);

    int pngColorType;
    switch (channels) {
        case 1: pngColorType = PNG_COLOR_TYPE_GRAY; break;
        case 2: pngColorType = PNG_COLOR_TYPE_GRAY_ALPHA; break;
        case 3: pngColorType = PNG_COLOR_TYPE_RGB; break;
        case 4: pngColorType = PNG_COLOR_TYPE_RGBA; break;
        default: throw std::invalid_argument(std::string(what) + ": unsupported channel count " +
                                             std::to_string(channels));
    }

    // Copy tensor data to a contiguous byte buffer
    const int64_t totalBytes = static_cast<int64_t>(height) * width * channels;
    std::vector<uint8_t> imgBuf(totalBytes);
    copyTensorToBytes(tensor, imgBuf.data(), totalBytes);
    std::vector<const png_byte*> rowPtrs(height);
    const size_t rowBytes = static_cast<size_t>(width) * channels;
    for (int y = 0; y < height; ++y)
        rowPtrs[y] = imgBuf.data() + y * rowBytes;
    std::vector<uint8_t> out;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) throw std::runtime_error(std::string(what) + ": png_create_write_struct failed");
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); throw std::runtime_error(std::string(what) + ": png_create_info_struct failed"); }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        throw std::runtime_error(std::string(what) + ": error encoding PNG");
    }

    png_set_write_fn(png, &out, &pngWriteMemory, &pngFlushMemory);
    png_set_IHDR(png, info, width, height, 8, pngColorType,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    png_write_image(png, const_cast<png_bytepp>(rowPtrs.data()));
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    return out;
}

// ---- JPEG ----

namespace {
// libjpeg's default error handler exits the process: this one jumps back
// instead, with the message.
struct JpegError {
    jpeg_error_mgr mgr;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};
}

static void jpegErrorExit(j_common_ptr cinfo)
{
    auto* err = reinterpret_cast<JpegError*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->jump, 1);
}

static void jpegOutputMessage(j_common_ptr) {}   // warnings: not on stderr

static Value decodeJPEG(const uint8_t* data, size_t size, int requestedChannels, const std::string& what)
{
    if (!isJPEG(data, size))
        throw std::runtime_error("media.Image: " + what + " is not a valid JPEG image");

    std::vector<uint8_t> imgBuf;
    std::vector<uint8_t> rowBuf;
    int width = 0, height = 0, outChannels = 0;

    jpeg_decompress_struct cinfo;
    JpegError jerr;
    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = &jpegErrorExit;
    jerr.mgr.output_message = &jpegOutputMessage;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        throw std::runtime_error("media.Image: error decoding JPEG " + what + ": " + jerr.message);
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, const_cast<unsigned char*>(data), static_cast<unsigned long>(size));
    jpeg_read_header(&cinfo, TRUE);

    if (requestedChannels == 1)
        cinfo.out_color_space = JCS_GRAYSCALE;
    else
        cinfo.out_color_space = JCS_RGB;

    jpeg_start_decompress(&cinfo);

    width = cinfo.output_width;
    height = cinfo.output_height;
    const int jpegChannels = cinfo.output_components;

    outChannels = (requestedChannels == 4) ? 4 : jpegChannels;
    if (requestedChannels > 0 && requestedChannels != 4)
        outChannels = requestedChannels;

    imgBuf.resize(static_cast<size_t>(height) * width * outChannels);

    if (requestedChannels == 4) {
        rowBuf.resize(static_cast<size_t>(width) * jpegChannels);
        for (int y = 0; y < height; ++y) {
            uint8_t* rowPtr = rowBuf.data();
            jpeg_read_scanlines(&cinfo, &rowPtr, 1);
            uint8_t* dst = imgBuf.data() + static_cast<size_t>(y) * width * 4;
            for (int x = 0; x < width; ++x) {
                dst[x * 4 + 0] = rowBuf[x * 3 + 0];
                dst[x * 4 + 1] = rowBuf[x * 3 + 1];
                dst[x * 4 + 2] = rowBuf[x * 3 + 2];
                dst[x * 4 + 3] = 255;
            }
        }
    } else {
        for (int y = 0; y < height; ++y) {
            uint8_t* rowPtr = imgBuf.data() + static_cast<size_t>(y) * width * outChannels;
            jpeg_read_scanlines(&cinfo, &rowPtr, 1);
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    std::vector<int64_t> shape = {height, width, outChannels};
    auto tensor = newTensorObj(shape, TensorDType::UInt8);
    copyBytesToTensor(tensor.get(), imgBuf.data(), static_cast<int64_t>(height) * width * outChannels);
    return Value::objVal(std::move(tensor));
}

static std::vector<uint8_t> encodeJPEG(ObjTensor* tensor, int quality, const char* what)
{
    int height, width, channels;
    encodableImage(tensor, what, "JPEG", height, width, channels);
    if (channels == 2 || channels == 4)
        throw std::invalid_argument(std::string(what) + ": JPEG does not support alpha channel (" +
                                    std::to_string(channels) + " channels)");
    if (channels != 1 && channels != 3)
        throw std::invalid_argument(std::string(what) + ": JPEG requires 1 or 3 channels, got " +
                                    std::to_string(channels));
    if (quality < 1 || quality > 100)
        throw std::invalid_argument(std::string(what) + ": quality must be 1-100");

    const int64_t totalBytes = static_cast<int64_t>(height) * width * channels;
    std::vector<uint8_t> imgBuf(totalBytes);
    copyTensorToBytes(tensor, imgBuf.data(), totalBytes);
    unsigned char* mem = nullptr;
    unsigned long memSize = 0;

    jpeg_compress_struct cinfo;
    JpegError jerr;
    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = &jpegErrorExit;
    jerr.mgr.output_message = &jpegOutputMessage;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_compress(&cinfo);
        free(mem);
        throw std::runtime_error(std::string(what) + ": error encoding JPEG: " + jerr.message);
    }
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &mem, &memSize);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = channels;
    cinfo.in_color_space = (channels == 1) ? JCS_GRAYSCALE : JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    for (int y = 0; y < height; ++y) {
        const uint8_t* rowPtr = imgBuf.data() + static_cast<size_t>(y) * width * channels;
        jpeg_write_scanlines(&cinfo, const_cast<JSAMPARRAY>(&rowPtr), 1);
    }

    jpeg_finish_compress(&cinfo);
    std::vector<uint8_t> out(mem, mem + memSize);
    jpeg_destroy_compress(&cinfo);
    free(mem);
    return out;
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
    linkMethod("Image", "write",           [this](VM&, ArgsView a) { return image_write_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Image", "encode",          [this](VM&, ArgsView a) { return image_encode_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Image", "width",           [this](VM&, ArgsView a) { return image_width_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Image", "height",          [this](VM&, ArgsView a) { return image_height_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Image", "channels",        [this](VM&, ArgsView a) { return image_channels_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Image", "resize",          [this](VM&, ArgsView a) { return image_resize_builtin(a); });
    linkMethod("Image", "crop",            [this](VM&, ArgsView a) { return image_crop_builtin(a); });
    linkMethod("Image", "pad",             [this](VM&, ArgsView a) { return image_pad_builtin(a); });
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
    linkMethod("Image", "normalize",       [this](VM&, ArgsView a) { return image_normalize_builtin(a); });
    linkMethod("Image", "to_tensor",       [this](VM&, ArgsView a) { return image_to_tensor_builtin(a); }, {}, 0, /*noMutateSelf=*/true);

#ifdef ROXAL_ENABLE_MEDIA_AUDIO
    registerAudioBuiltins();
#else
    // Images without audio (the wasm build). audio_available() still answers,
    // because a script that asks "can I play sound here?" deserves `false`
    // rather than the raise an unlinked builtin would give it -- the whole
    // point of asking is to take the other branch.
    link("audio_available", [](VM&, ArgsView) { return Value::falseVal(); });
#endif
}

#ifndef ROXAL_ENABLE_MEDIA_AUDIO
// The audio half owns this hook (it shuts the engine down); with no engine
// there is nothing to unload.
void ModuleMedia::onModuleUnloading(VM&) {}
#endif

// Helper: replace the receiver's data tensor in-place
static void setImageData(ArgsView args, Value tensorVal)
{
    asObjectInstance(args[0])->setProperty("data", tensorVal);
}

// The bytes of an encoded image given as a packed byte list (or a list of
// bytes) or a 1-D uint8 tensor.
static std::vector<uint8_t> encodedBytesArg(const Value& v, const char* what)
{
    if (isTensor(v)) {
        ObjTensor* t = asTensor(v);
        if (t->dtype() != TensorDType::UInt8 || t->rank() != 1)
            throw std::invalid_argument(std::string(what) + ": bytes must be a list of bytes or a 1-D uint8 tensor");
        const auto* data = static_cast<const uint8_t*>(t->rawData());
        return std::vector<uint8_t>(data, data + t->numel());
    }
    if (!isList(v))
        throw std::invalid_argument(std::string(what) + ": bytes must be a list of bytes or a 1-D uint8 tensor");
    ObjList* list = asList(v);
    if (const auto* packed = list->packedBytes())
        return *packed;
    std::vector<uint8_t> bytes;
    bytes.reserve(list->length());
    for (int32_t i = 0; i < list->length(); ++i) {
        const Value b = list->getElement(i);
        if (!b.isByte())
            throw std::invalid_argument(std::string(what) + ": bytes must be a list of bytes or a 1-D uint8 tensor");
        bytes.push_back(b.asByte());
    }
    return bytes;
}

// ============================================================
// Image.init(path, tensor, channels, bytes)
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

    // Get encoded bytes argument (args[4])
    const bool hasBytes = args.size() >= 5 && !args[4].isNil();

    if ((!path.empty()) + hasTensor + hasBytes > 1)
        throw std::invalid_argument("Image.init: provide one of path, tensor or bytes");

    if (hasBytes) {
        const std::vector<uint8_t> bytes = encodedBytesArg(args[4], "Image.init");
        Value tensorVal;
        if (isPNG(bytes.data(), bytes.size()))
            tensorVal = decodePNG(bytes.data(), bytes.size(), requestedChannels, "bytes");
        else if (isJPEG(bytes.data(), bytes.size()))
            tensorVal = decodeJPEG(bytes.data(), bytes.size(), requestedChannels, "bytes");
        else
            throw std::invalid_argument("Image.init: bytes are not a PNG or JPEG image");
        inst->setProperty("data", tensorVal);
        return Value::nilVal();
    }

    if (hasTensor) {
        ObjTensor* t = asTensor(args[2]);
        if (t->rank() != 3)
            throw std::invalid_argument("Image.init: tensor must be 3D [H, W, C], got rank " +
                                        std::to_string(t->rank()));
        inst->setProperty("data", args[2]);
        return Value::nilVal();
    }

    if (path.empty())
        throw std::invalid_argument("Image.init: provide a file path, tensor or bytes");

    std::string ext = toLower(std::filesystem::path(path).extension().string());

    Value tensorVal;
    if (ext == ".png") {
        const auto bytes = readFileBytes(path, "media.Image");
        tensorVal = decodePNG(bytes.data(), bytes.size(), requestedChannels, "'" + path + "'");
    } else if (ext == ".jpg" || ext == ".jpeg") {
        const auto bytes = readFileBytes(path, "media.Image");
        tensorVal = decodeJPEG(bytes.data(), bytes.size(), requestedChannels, "'" + path + "'");
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
        writeFileBytes(path, encodePNG(tensor, "Image.write"));
    } else if (ext == ".jpg" || ext == ".jpeg") {
        writeFileBytes(path, encodeJPEG(tensor, quality, "Image.write"));
    } else {
        throw std::invalid_argument("Image.write: unsupported format '" + ext +
                                    "' (supported: .png, .jpg, .jpeg)");
    }

    return Value::boolVal(true);
}

// ============================================================
// Image.encode(format, quality) -> list (packed bytes)
// ============================================================

Value ModuleMedia::image_encode_builtin(ArgsView args)
{
    ObjTensor* tensor = getImageTensor(args, "encode");
    std::string format = "jpeg";
    if (args.size() >= 2 && !args[1].isNil()) {
        if (!isString(args[1]))
            throw std::invalid_argument("Image.encode: format must be 'jpeg' or 'png'");
        format = toLower(toUTF8StdString(asStringObj(args[1])->s));
    }
    int quality = 95;
    if (args.size() >= 3 && !args[2].isNil())
        quality = static_cast<int>(toType(ValueType::Int, args[2], false).asInt());

    if (format == "jpeg" || format == "jpg")
        return Value::listVal(encodeJPEG(tensor, quality, "Image.encode"));
    if (format == "png")
        return Value::listVal(encodePNG(tensor, "Image.encode"));
    throw std::invalid_argument("Image.encode: format must be 'jpeg' or 'png', not '" + format + "'");
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
    PixelsIn in(src);
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
    PixelsOut out(dst.get());

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

                double v00 = in[i00], v01 = in[i01];
                double v10 = in[i10], v11 = in[i11];

                double val = (1 - fy) * ((1 - fx) * v00 + fx * v01) +
                             fy * ((1 - fx) * v10 + fx * v11);

                int di = (y * dstW + x) * ch + c;
                if (dtype == TensorDType::UInt8) {
                    out.set(di, std::clamp(std::round(val), 0.0, 255.0));
                } else {
                    out.set(di, val);
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
    PixelsIn in(src);
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
    PixelsOut out(dst.get());

    for (int y = 0; y < cropH; ++y)
        for (int x = 0; x < cw; ++x)
            for (int c = 0; c < channels; ++c)
                out.set((y * cw + x) * channels + c,
                        in[((cy + y) * srcW + (cx + x)) * channels + c]);

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Pad (zero-pad to target size, original at top-left)
// ============================================================

Value ModuleMedia::image_pad_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "pad");
    PixelsIn in(src);
    if (args.size() < 3)
        throw std::invalid_argument("Image.pad expects width, height");

    int padW = static_cast<int>(toType(ValueType::Int, args[1], false).asInt());
    int padH = static_cast<int>(toType(ValueType::Int, args[2], false).asInt());

    int srcH = static_cast<int>(src->shape()[0]);
    int srcW = static_cast<int>(src->shape()[1]);
    int channels = static_cast<int>(src->shape()[2]);

    if (padW < srcW || padH < srcH)
        throw std::invalid_argument("Image.pad: target size must be >= current size");

    TensorDType dtype = src->dtype();
    std::vector<int64_t> dstShape = {padH, padW, channels};
    auto dst = newTensorObj(dstShape, dtype); // zero-filled by default
    PixelsOut out(dst.get());

    // Copy original image pixels into top-left corner
    for (int y = 0; y < srcH; ++y)
        for (int x = 0; x < srcW; ++x)
            for (int c = 0; c < channels; ++c)
                out.set((y * padW + x) * channels + c,
                        in[(y * srcW + x) * channels + c]);

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Flip horizontal
// ============================================================

Value ModuleMedia::image_flip_horizontal_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "flip_horizontal");
    PixelsIn in(src);
    int h = static_cast<int>(src->shape()[0]);
    int w = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    auto dst = newTensorObj(src->shape(), src->dtype());

    PixelsOut out(dst.get());

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c)
                out.set((y * w + (w - 1 - x)) * ch + c,
                        in[(y * w + x) * ch + c]);

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Flip vertical
// ============================================================

Value ModuleMedia::image_flip_vertical_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "flip_vertical");
    PixelsIn in(src);
    int h = static_cast<int>(src->shape()[0]);
    int w = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    auto dst = newTensorObj(src->shape(), src->dtype());

    PixelsOut out(dst.get());

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c)
                out.set(((h - 1 - y) * w + x) * ch + c,
                        in[(y * w + x) * ch + c]);

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Rotate 90 degrees clockwise
// ============================================================

Value ModuleMedia::image_rotate90_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "rotate90");
    PixelsIn in(src);
    int h = static_cast<int>(src->shape()[0]);
    int w = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    // Output: [W, H, C]
    std::vector<int64_t> dstShape = {w, h, ch};
    auto dst = newTensorObj(dstShape, src->dtype());
    PixelsOut out(dst.get());

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c)
                out.set((x * h + (h - 1 - y)) * ch + c,
                        in[(y * w + x) * ch + c]);

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Rotate 180 degrees
// ============================================================

Value ModuleMedia::image_rotate180_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "rotate180");
    PixelsIn in(src);
    int h = static_cast<int>(src->shape()[0]);
    int w = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    auto dst = newTensorObj(src->shape(), src->dtype());

    PixelsOut out(dst.get());

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c)
                out.set(((h - 1 - y) * w + (w - 1 - x)) * ch + c,
                        in[(y * w + x) * ch + c]);

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Rotate 270 degrees clockwise (= 90 counter-clockwise)
// ============================================================

Value ModuleMedia::image_rotate270_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "rotate270");
    PixelsIn in(src);
    int h = static_cast<int>(src->shape()[0]);
    int w = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    // Output: [W, H, C]
    std::vector<int64_t> dstShape = {w, h, ch};
    auto dst = newTensorObj(dstShape, src->dtype());
    PixelsOut out(dst.get());

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c)
                out.set(((w - 1 - x) * h + y) * ch + c,
                        in[(y * w + x) * ch + c]);

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// Grayscale
// ============================================================

Value ModuleMedia::image_grayscale_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "grayscale");
    PixelsIn in(src);
    int h = static_cast<int>(src->shape()[0]);
    int w = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    if (ch != 3 && ch != 4)
        throw std::invalid_argument("Image.grayscale: requires RGB (3) or RGBA (4) channels, got " +
                                    std::to_string(ch));

    std::vector<int64_t> dstShape = {h, w, 1};
    auto dst = newTensorObj(dstShape, src->dtype());
    PixelsOut out(dst.get());

    for (int i = 0; i < h * w; ++i) {
        double r = in[i * ch + 0];
        double g = in[i * ch + 1];
        double b = in[i * ch + 2];
        double gray = 0.299 * r + 0.587 * g + 0.114 * b;
        if (src->dtype() == TensorDType::UInt8)
            gray = std::clamp(std::round(gray), 0.0, 255.0);
        out.set(i, gray);
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
    PixelsIn in(src);
    if (args.size() < 2)
        throw std::invalid_argument("Image.brightness expects factor");
    double factor = toType(ValueType::Real, args[1], false).asReal();

    int64_t total = src->numel();
    auto dst = newTensorObj(src->shape(), src->dtype());
    PixelsOut out(dst.get());

    double maxVal = (src->dtype() == TensorDType::UInt8) ? 255.0 : 1.0;
    for (int64_t i = 0; i < total; ++i) {
        double val = std::clamp(in[i] * factor, 0.0, maxVal);
        if (src->dtype() == TensorDType::UInt8)
            val = std::round(val);
        out.set(i, val);
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
    PixelsIn in(src);
    if (args.size() < 2)
        throw std::invalid_argument("Image.contrast expects factor");
    double factor = toType(ValueType::Real, args[1], false).asReal();

    int64_t total = src->numel();

    // Compute mean intensity
    double sum = 0;
    for (int64_t i = 0; i < total; ++i)
        sum += in[i];
    double mean = sum / total;

    double maxVal = (src->dtype() == TensorDType::UInt8) ? 255.0 : 1.0;
    auto dst = newTensorObj(src->shape(), src->dtype());
    PixelsOut out(dst.get());

    for (int64_t i = 0; i < total; ++i) {
        double val = std::clamp(mean + factor * (in[i] - mean), 0.0, maxVal);
        if (src->dtype() == TensorDType::UInt8)
            val = std::round(val);
        out.set(i, val);
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
    PixelsIn in(src);
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
    PixelsOut out(dst.get());

    for (int i = 0; i < h * w; ++i) {
        double r = in[i * ch + 0];
        double g = in[i * ch + 1];
        double b = in[i * ch + 2];
        double gray = 0.299 * r + 0.587 * g + 0.114 * b;

        auto adjust = [&](double v) -> double {
            double val = std::clamp(gray + factor * (v - gray), 0.0, maxVal);
            return (src->dtype() == TensorDType::UInt8) ? std::round(val) : val;
        };

        out.set(i * ch + 0, adjust(r));
        out.set(i * ch + 1, adjust(g));
        out.set(i * ch + 2, adjust(b));
        // Copy alpha if present
        if (ch == 4)
            out.set(i * ch + 3, in[i * ch + 3]);
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
    PixelsIn in(src);
    if (src->dtype() != TensorDType::UInt8)
        throw std::invalid_argument("Image.to_float: tensor must be uint8");

    int64_t total = src->numel();
    auto dst = newTensorObj(src->shape(), TensorDType::Float32);
    PixelsOut out(dst.get());

    for (int64_t i = 0; i < total; ++i)
        out.set(i, in[i] / 255.0);

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// to_uint8: float32/float64 [0.0-1.0] -> uint8 [0-255]
// ============================================================

Value ModuleMedia::image_to_uint8_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "to_uint8");
    PixelsIn in(src);
    if (src->dtype() != TensorDType::Float32 && src->dtype() != TensorDType::Float64)
        throw std::invalid_argument("Image.to_uint8: tensor must be float32 or float64");

    int64_t total = src->numel();
    auto dst = newTensorObj(src->shape(), TensorDType::UInt8);
    PixelsOut out(dst.get());

    for (int64_t i = 0; i < total; ++i) {
        double val = in[i] * 255.0;
        out.set(i, std::clamp(std::round(val), 0.0, 255.0));
    }

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// normalize: per-channel (pixel - mean[c]) / std[c], in-place
// ============================================================

// Helper: extract a list of exactly `n` doubles from a Value (must be a list)
static std::vector<double> extractDoubleList(Value v, int n, const char* name)
{
    if (!isList(v))
        throw std::invalid_argument(std::string(name) + " must be a list");
    ObjList* lst = asList(v);
    if (lst->length() != n)
        throw std::invalid_argument(std::string(name) + " must have " +
                                    std::to_string(n) + " elements, got " +
                                    std::to_string(lst->length()));
    std::vector<double> result(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        result[static_cast<size_t>(i)] = toType(ValueType::Real, lst->getElement(i), false).asReal();
    return result;
}

Value ModuleMedia::image_normalize_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "normalize");
    PixelsIn in(src);
    if (src->dtype() != TensorDType::Float32 && src->dtype() != TensorDType::Float64)
        throw std::invalid_argument("Image.normalize: image must be float (call to_float() first)");
    if (args.size() < 3)
        throw std::invalid_argument("Image.normalize expects mean and std arguments");

    int h  = static_cast<int>(src->shape()[0]);
    int w  = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);

    auto mean = extractDoubleList(args[1], ch, "mean");
    auto std  = extractDoubleList(args[2], ch, "std");

    auto dst = newTensorObj(src->shape(), src->dtype());

    PixelsOut out(dst.get());

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int base = (y * w + x) * ch;
            for (int c = 0; c < ch; ++c) {
                double val = (in[base + c] - mean[static_cast<size_t>(c)])
                           / std[static_cast<size_t>(c)];
                out.set(base + c, val);
            }
        }
    }

    setImageData(args, Value::objVal(std::move(dst)));
    return Value::nilVal();
}

// ============================================================
// to_tensor: [H, W, C] -> float32 [1, C, H, W] for NN input
// Optional mean/std for per-channel normalization.
// ============================================================

Value ModuleMedia::image_to_tensor_builtin(ArgsView args)
{
    ObjTensor* src = getImageTensor(args, "to_tensor");
    PixelsIn in(src);

    int h  = static_cast<int>(src->shape()[0]);
    int w  = static_cast<int>(src->shape()[1]);
    int ch = static_cast<int>(src->shape()[2]);
    bool isUint8 = (src->dtype() == TensorDType::UInt8);

    // Optional mean/std normalization (args[1]=mean, args[2]=std)
    bool hasNorm = false;
    std::vector<double> mean, stddev;
    if (args.size() >= 3 && !args[1].isNil() && !args[2].isNil()) {
        mean   = extractDoubleList(args[1], ch, "mean");
        stddev = extractDoubleList(args[2], ch, "std");
        hasNorm = true;
    } else if (args.size() >= 2 && !args[1].isNil()) {
        throw std::invalid_argument("Image.to_tensor: must provide both mean and std, or neither");
    }

    std::vector<int64_t> dstShape = {1, ch, h, w};
    auto dst = newTensorObj(dstShape, TensorDType::Float32);
    PixelsOut out(dst.get());

    // HWC → NCHW with /255 normalization for uint8
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int srcBase = (y * w + x) * ch;
            for (int c = 0; c < ch; ++c) {
                double val = in[srcBase + c];
                if (isUint8)
                    val /= 255.0;  // else already float

                if (hasNorm)
                    val = (val - mean[static_cast<size_t>(c)]) / stddev[static_cast<size_t>(c)];

                int dstIdx = ((c * h) + y) * w + x;  // NCHW with N=0
                out.set(dstIdx, val);
            }
        }
    }

    return Value::objVal(std::move(dst));
}

