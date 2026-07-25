// rs_shim: flat C ABI over librealsense2 for the Roxal realsense module
// (loaded via sys.loadlib).
//
// Why a shim at all: every rs2_* entry point reports failure through a trailing
// `rs2_error**` out-parameter, and Roxal's FFI has no way to receive an opaque
// handle written through a pointer. This layer absorbs that convention (and the
// frame/queue/profile lifetime rules) and presents the plain-scalar,
// caller-allocated-buffer style the other Roxal FFI modules use.
//
// Conventions (matching modules/opencv/shim/cvx_shim.cpp):
//  - int-returning functions: 0/1 = ok, negative = error (message via rs_last_error).
//    Pointer-returning functions: NULL = error.
//  - Images cross the boundary as caller-allocated contiguous buffers: depth as
//    uint16 [H, W, 1] in raw sensor units, colour as uint8 [H, W, 3] RGB (the
//    camera is asked for RGB8 directly, so no channel swap happens anywhere).
//  - rs_stop() releases the device now; the handle itself is freed later by
//    rs_close(), which Roxal registers as the @cfunc free= finalizer. Calling
//    both — in either order — is safe.

#include <librealsense2/rs.h>
#include <librealsense2/rsutil.h>            // rs2_deproject_pixel_to_point (exported by the SDK)
#include <librealsense2/h/rs_pipeline.h>
#include <librealsense2/h/rs_option.h>
#include <librealsense2/h/rs_frame.h>
#include <librealsense2/h/rs_processing.h>

#include <cstdint>
#include <cstring>
#include <string>

// The Roxal-side declarations in init.rox are written against this API surface.
static_assert(RS2_API_MAJOR_VERSION == 2 && RS2_API_MINOR_VERSION >= 58,
              "rs_shim is written against librealsense 2.58+ — review init.rox before building against an older SDK");

static thread_local std::string g_lastError;

extern "C" const char* rs_last_error(void) { return g_lastError.c_str(); }
extern "C" const char* rs_version(void) { return RS2_API_VERSION_STR; }

// Consume an rs2_error*: record its message and free it. Returns true on error.
static bool failed(rs2_error* e, const char* what)
{
    if (!e)
        return false;
    const char* msg = rs2_get_error_message(e);
    g_lastError = std::string(what) + ": " + (msg ? msg : "unknown librealsense error");
    rs2_free_error(e);
    return true;
}

//
// context + device enumeration

extern "C" void rs_delete_context(void* ctx)
{
    if (ctx)
        rs2_delete_context(static_cast<rs2_context*>(ctx));
}

extern "C" void* rs_create_context(void)
{
    rs2_error* e = nullptr;
    rs2_context* ctx = rs2_create_context(RS2_API_VERSION, &e);
    if (failed(e, "create_context"))
        return nullptr;
    return ctx;
}

// Number of connected devices, or negative on error.
extern "C" int rs_device_count(void* ctx)
{
    rs2_error* e = nullptr;
    rs2_device_list* list = rs2_query_devices(static_cast<rs2_context*>(ctx), &e);
    if (failed(e, "query_devices"))
        return -1;
    int n = rs2_get_device_count(list, &e);
    rs2_delete_device_list(list);
    if (failed(e, "get_device_count"))
        return -1;
    return n;
}

// One rs2_camera_info string for device `index`, or NULL if the device does not
// report that field (Roxal turns NULL into nil). The result points at a
// thread-local buffer, valid until this thread's next rs_device_info call.
extern "C" const char* rs_device_info(void* ctx, int index, int info)
{
    static thread_local std::string result;
    rs2_error* e = nullptr;
    rs2_device_list* list = rs2_query_devices(static_cast<rs2_context*>(ctx), &e);
    if (failed(e, "query_devices"))
        return nullptr;
    rs2_device* dev = rs2_create_device(list, index, &e);
    rs2_delete_device_list(list);
    if (failed(e, "create_device"))
        return nullptr;

    const char* s = rs2_get_device_info(dev, static_cast<rs2_camera_info>(info), &e);
    if (failed(e, "get_device_info")) {          // field unsupported on this model
        rs2_delete_device(dev);
        return nullptr;
    }
    result = s ? s : "";
    rs2_delete_device(dev);
    return result.c_str();
}

// Hardware-reset device `index` (it drops off the bus and re-enumerates).
extern "C" int rs_device_reset(void* ctx, int index)
{
    rs2_error* e = nullptr;
    rs2_device_list* list = rs2_query_devices(static_cast<rs2_context*>(ctx), &e);
    if (failed(e, "query_devices"))
        return -1;
    rs2_device* dev = rs2_create_device(list, index, &e);
    rs2_delete_device_list(list);
    if (failed(e, "create_device"))
        return -1;
    rs2_hardware_reset(dev, &e);
    rs2_delete_device(dev);
    if (failed(e, "hardware_reset"))
        return -1;
    return 0;
}

//
// streaming session
//
// One handle owns the pipeline, the optional depth->colour aligner and a copy of
// the negotiated calibration, so the Roxal side never has to juggle frame, queue
// or profile lifetimes: rs_read() waits, aligns, copies into caller tensors and
// releases. Calibration is *copied* at open time because the rs2_stream_profile
// pointers belong to the profile list and must not outlive it.

namespace {

// Size-preserving depth filters, in the order librealsense recommends applying
// them. Decimation is deliberately absent: it changes the depth resolution, so
// it would have to renegotiate every cached size and the aligner's geometry.
enum FilterId { FILTER_SPATIAL = 0, FILTER_TEMPORAL = 1, FILTER_HOLE_FILLING = 2, FILTER_COUNT = 3 };

struct Session
{
    rs2_pipeline*         pipe    = nullptr;
    rs2_config*           config  = nullptr;
    rs2_pipeline_profile* profile = nullptr;
    rs2_processing_block* align   = nullptr;
    rs2_frame_queue*      queue   = nullptr;
    bool                  started = false;

    rs2_sensor* depthSensor = nullptr;
    rs2_sensor* colorSensor = nullptr;

    int    depthW = 0, depthH = 0;
    int    colorW = 0, colorH = 0;
    int    irW = 0, irH = 0;
    double depthScale = 0.0;                   // metres per raw unit
    double lastTimestamp = 0.0;                // ms, of the most recent frameset

    rs2_intrinsics depthIntr{}, colorIntr{};
    bool           hasDepthIntr = false, hasColorIntr = false;
    rs2_extrinsics depthToColor{};
    bool           hasExtrinsics = false;

    // Depth post-processing chain: each enabled block gets its own queue and is
    // applied to the whole frameset (non-depth frames pass through untouched).
    rs2_processing_block* filters[FILTER_COUNT]      = {nullptr, nullptr, nullptr};
    rs2_frame_queue*      filterQueues[FILTER_COUNT] = {nullptr, nullptr, nullptr};
    bool                  filterEnabled[FILTER_COUNT] = {false, false, false};

    // Point cloud: built lazily on first use, from the frames retained by the
    // last rs_read so the cloud always matches the frameset the caller has.
    rs2_processing_block* pointcloud = nullptr;
    rs2_frame_queue*      pcQueue    = nullptr;
    rs2_frame*            points     = nullptr;   // most recent computed cloud
    rs2_frame*            lastDepth  = nullptr;   // retained from the last read
    rs2_frame*            lastColor  = nullptr;

    // Motion samples from the most recent read (the IMU runs faster than the
    // video streams, so a frameset carries the latest sample, not one per frame).
    float gyro[3]  = {0, 0, 0};
    float accel[3] = {0, 0, 0};
    bool  hasGyro = false, hasAccel = false;
};

void releaseFrame(rs2_frame*& f)
{
    if (f) {
        rs2_release_frame(f);
        f = nullptr;
    }
}

// Find the sensor supporting `extension` on `dev`, or NULL.
rs2_sensor* findSensor(rs2_device* dev, rs2_extension extension)
{
    rs2_error* e = nullptr;
    rs2_sensor_list* sensors = rs2_query_sensors(dev, &e);
    if (failed(e, "query_sensors"))
        return nullptr;
    int n = rs2_get_sensors_count(sensors, &e);
    if (failed(e, "get_sensors_count")) {
        rs2_delete_sensor_list(sensors);
        return nullptr;
    }
    rs2_sensor* found = nullptr;
    for (int i = 0; i < n && !found; i++) {
        rs2_sensor* s = rs2_create_sensor(sensors, i, &e);
        if (failed(e, "create_sensor"))
            continue;
        int is = rs2_is_sensor_extendable_to(s, extension, &e);
        if (!failed(e, "is_sensor_extendable_to") && is)
            found = s;
        else
            rs2_delete_sensor(s);
    }
    rs2_delete_sensor_list(sensors);
    return found;
}

// Release device resources now. Idempotent, so an explicit close() from Roxal
// followed by the GC finalizer (or vice versa) is harmless.
void stopSession(Session* s)
{
    if (!s)
        return;
    if (s->pipe && s->started) {
        rs2_error* e = nullptr;
        rs2_pipeline_stop(s->pipe, &e);
        if (e)
            rs2_free_error(e);                 // best effort on teardown
        s->started = false;
    }
    releaseFrame(s->points);
    releaseFrame(s->lastDepth);
    releaseFrame(s->lastColor);
    if (s->pcQueue)     { rs2_delete_frame_queue(s->pcQueue);          s->pcQueue = nullptr; }
    if (s->pointcloud)  { rs2_delete_processing_block(s->pointcloud);  s->pointcloud = nullptr; }
    for (int i = 0; i < FILTER_COUNT; i++) {
        if (s->filterQueues[i]) { rs2_delete_frame_queue(s->filterQueues[i]);         s->filterQueues[i] = nullptr; }
        if (s->filters[i])      { rs2_delete_processing_block(s->filters[i]);         s->filters[i] = nullptr; }
        s->filterEnabled[i] = false;
    }
    if (s->queue)       { rs2_delete_frame_queue(s->queue);        s->queue = nullptr; }
    if (s->align)       { rs2_delete_processing_block(s->align);   s->align = nullptr; }
    if (s->depthSensor) { rs2_delete_sensor(s->depthSensor);       s->depthSensor = nullptr; }
    if (s->colorSensor) { rs2_delete_sensor(s->colorSensor);       s->colorSensor = nullptr; }
    if (s->profile)     { rs2_delete_pipeline_profile(s->profile); s->profile = nullptr; }
    if (s->config)      { rs2_delete_config(s->config);            s->config = nullptr; }
    if (s->pipe)        { rs2_delete_pipeline(s->pipe);            s->pipe = nullptr; }
}

} // namespace

// Release the camera now (emitter off, device free for other processes); the
// handle itself is freed later by the GC finalizer.
extern "C" void rs_stop(void* h) { stopSession(static_cast<Session*>(h)); }

extern "C" void rs_close(void* h)
{
    Session* s = static_cast<Session*>(h);
    stopSession(s);
    delete s;
}

// Start streaming. serial may be NULL/empty for "first device found"; a 0 width,
// height or fps asks librealsense for that stream's default. align_to_color
// reprojects depth onto the colour image (requires both streams).
extern "C" void* rs_open(void* ctx,
                         const char* serial,
                         int enable_depth, int depth_w, int depth_h,
                         int enable_color, int color_w, int color_h,
                         int enable_ir, int enable_imu,
                         int fps, int align_to_color)
{
    if (!enable_depth && !enable_color && !enable_ir && !enable_imu) {
        g_lastError = "open: no streams requested";
        return nullptr;
    }
    if (align_to_color && !(enable_depth && enable_color)) {
        g_lastError = "open: align requires both depth and colour streams";
        return nullptr;
    }

    Session* s = new Session();
    rs2_error* e = nullptr;

    s->pipe = rs2_create_pipeline(static_cast<rs2_context*>(ctx), &e);
    if (failed(e, "create_pipeline")) { rs_close(s); return nullptr; }

    s->config = rs2_create_config(&e);
    if (failed(e, "create_config")) { rs_close(s); return nullptr; }

    if (serial && *serial) {
        rs2_config_enable_device(s->config, serial, &e);
        if (failed(e, "enable_device")) { rs_close(s); return nullptr; }
    }
    if (enable_depth) {
        rs2_config_enable_stream(s->config, RS2_STREAM_DEPTH, -1,
                                 depth_w, depth_h, RS2_FORMAT_Z16, fps, &e);
        if (failed(e, "enable_stream(depth)")) { rs_close(s); return nullptr; }
    }
    if (enable_color) {
        rs2_config_enable_stream(s->config, RS2_STREAM_COLOR, -1,
                                 color_w, color_h, RS2_FORMAT_RGB8, fps, &e);
        if (failed(e, "enable_stream(color)")) { rs_close(s); return nullptr; }
    }
    if (enable_ir) {
        // Index 1 is the left imager, which shares the depth stream's viewpoint.
        rs2_config_enable_stream(s->config, RS2_STREAM_INFRARED, 1,
                                 depth_w, depth_h, RS2_FORMAT_Y8, fps, &e);
        if (failed(e, "enable_stream(infrared)")) { rs_close(s); return nullptr; }
    }
    if (enable_imu) {
        // Motion streams carry no image geometry; 0 fps asks for the default rate.
        rs2_config_enable_stream(s->config, RS2_STREAM_GYRO, 0, 0, 0,
                                 RS2_FORMAT_MOTION_XYZ32F, 0, &e);
        if (failed(e, "enable_stream(gyro)")) { rs_close(s); return nullptr; }
        rs2_config_enable_stream(s->config, RS2_STREAM_ACCEL, 0, 0, 0,
                                 RS2_FORMAT_MOTION_XYZ32F, 0, &e);
        if (failed(e, "enable_stream(accel)")) { rs_close(s); return nullptr; }
    }

    s->profile = rs2_pipeline_start_with_config(s->pipe, s->config, &e);
    if (failed(e, "pipeline_start")) { rs_close(s); return nullptr; }
    s->started = true;

    // Read back what was actually negotiated (the driver may snap the request)
    // and copy the calibration out while the profile list is alive.
    rs2_stream_profile_list* streams = rs2_pipeline_profile_get_streams(s->profile, &e);
    if (failed(e, "get_streams")) { rs_close(s); return nullptr; }
    int nstreams = rs2_get_stream_profiles_count(streams, &e);
    if (failed(e, "get_stream_profiles_count")) {
        rs2_delete_stream_profiles_list(streams);
        rs_close(s);
        return nullptr;
    }

    const rs2_stream_profile* depthProfile = nullptr;
    const rs2_stream_profile* colorProfile = nullptr;
    for (int i = 0; i < nstreams; i++) {
        const rs2_stream_profile* p = rs2_get_stream_profile(streams, i, &e);
        if (failed(e, "get_stream_profile"))
            continue;
        rs2_stream stream = RS2_STREAM_ANY;
        rs2_format format = RS2_FORMAT_ANY;
        int index = 0, uid = 0, framerate = 0;
        rs2_get_stream_profile_data(p, &stream, &format, &index, &uid, &framerate, &e);
        if (failed(e, "get_stream_profile_data"))
            continue;
        if (stream != RS2_STREAM_DEPTH && stream != RS2_STREAM_COLOR && stream != RS2_STREAM_INFRARED)
            continue;                          // motion profiles have no resolution
        int w = 0, h = 0;
        rs2_get_video_stream_resolution(p, &w, &h, &e);
        if (failed(e, "get_video_stream_resolution"))
            continue;
        if (stream == RS2_STREAM_DEPTH) {
            s->depthW = w; s->depthH = h; depthProfile = p;
        } else if (stream == RS2_STREAM_COLOR) {
            s->colorW = w; s->colorH = h; colorProfile = p;
        } else if (stream == RS2_STREAM_INFRARED) {
            s->irW = w; s->irH = h;
        }
    }
    if (depthProfile) {
        rs2_get_video_stream_intrinsics(depthProfile, &s->depthIntr, &e);
        s->hasDepthIntr = !failed(e, "get_video_stream_intrinsics(depth)");
    }
    if (colorProfile) {
        rs2_get_video_stream_intrinsics(colorProfile, &s->colorIntr, &e);
        s->hasColorIntr = !failed(e, "get_video_stream_intrinsics(color)");
    }
    if (depthProfile && colorProfile) {
        rs2_get_extrinsics(depthProfile, colorProfile, &s->depthToColor, &e);
        s->hasExtrinsics = !failed(e, "get_extrinsics");
    }
    rs2_delete_stream_profiles_list(streams);

    // Depth scale + sensor handles (for option get/set) from the running device.
    rs2_device* dev = rs2_pipeline_profile_get_device(s->profile, &e);
    if (failed(e, "get_device")) { rs_close(s); return nullptr; }
    s->depthSensor = findSensor(dev, RS2_EXTENSION_DEPTH_SENSOR);
    s->colorSensor = findSensor(dev, RS2_EXTENSION_COLOR_SENSOR);
    if (s->depthSensor) {
        float scale = rs2_get_depth_scale(s->depthSensor, &e);
        if (failed(e, "get_depth_scale")) { rs2_delete_device(dev); rs_close(s); return nullptr; }
        s->depthScale = scale;
    }
    rs2_delete_device(dev);

    if (align_to_color) {
        s->align = rs2_create_align(RS2_STREAM_COLOR, &e);
        if (failed(e, "create_align")) { rs_close(s); return nullptr; }
        s->queue = rs2_create_frame_queue(1, &e);
        if (failed(e, "create_frame_queue")) { rs_close(s); return nullptr; }
        rs2_start_processing_queue(s->align, s->queue, &e);
        if (failed(e, "start_processing_queue")) { rs_close(s); return nullptr; }
        // Aligned depth lives in the colour image's geometry and calibration.
        s->depthW = s->colorW;
        s->depthH = s->colorH;
        if (s->hasColorIntr) {
            s->depthIntr = s->colorIntr;
            s->hasDepthIntr = true;
        }
    }

    return s;
}

extern "C" int    rs_depth_width(void* h)  { return h ? static_cast<Session*>(h)->depthW : 0; }
extern "C" int    rs_depth_height(void* h) { return h ? static_cast<Session*>(h)->depthH : 0; }
extern "C" int    rs_color_width(void* h)  { return h ? static_cast<Session*>(h)->colorW : 0; }
extern "C" int    rs_color_height(void* h) { return h ? static_cast<Session*>(h)->colorH : 0; }
extern "C" int    rs_ir_width(void* h)     { return h ? static_cast<Session*>(h)->irW : 0; }
extern "C" int    rs_ir_height(void* h)    { return h ? static_cast<Session*>(h)->irH : 0; }
extern "C" double rs_depth_scale(void* h)  { return h ? static_cast<Session*>(h)->depthScale : 0.0; }
extern "C" double rs_timestamp(void* h)    { return h ? static_cast<Session*>(h)->lastTimestamp : 0.0; }

namespace {

// Copy one frame's pixels into a caller buffer, guarding against a size mismatch.
bool copyFrame(rs2_frame* f, void* dst, size_t dstBytes, const char* what)
{
    rs2_error* e = nullptr;
    const void* data = rs2_get_frame_data(f, &e);
    if (failed(e, what))
        return false;
    int size = rs2_get_frame_data_size(f, &e);
    if (failed(e, what))
        return false;
    if (size < 0 || static_cast<size_t>(size) != dstBytes) {
        g_lastError = std::string(what) + ": frame is " + std::to_string(size) +
                      " bytes but the destination buffer is " + std::to_string(dstBytes);
        return false;
    }
    std::memcpy(dst, data, dstBytes);
    return true;
}

} // namespace

// Wait for the next frameset and copy it into the caller's tensors. Either
// destination may be NULL to skip that stream. Returns 1 on success, 0 if no
// frameset arrived within timeout_ms, negative on error.
extern "C" int rs_read(void* h, void* depth_dst, void* color_dst, void* ir_dst, int timeout_ms)
{
    Session* s = static_cast<Session*>(h);
    if (!s || !s->pipe || !s->started) {
        g_lastError = "read: camera is closed";
        return -1;
    }

    rs2_error* e = nullptr;
    rs2_frame* frames = rs2_pipeline_wait_for_frames(s->pipe, static_cast<unsigned>(timeout_ms), &e);
    if (e) {
        // A timeout is an ordinary outcome for a script polling a camera, not a
        // failure: report it as "no frame" and keep g_lastError for real errors.
        const char* msg = rs2_get_error_message(e);
        bool timedOut = msg && std::strstr(msg, "Frame didn't arrive");
        if (timedOut) {
            rs2_free_error(e);
            return 0;
        }
        (void)failed(e, "wait_for_frames");
        return -1;
    }

    // Depth post-processing runs before alignment, on the whole frameset: each
    // block passes non-depth frames through untouched. rs2_process_frame takes
    // ownership of its input, so `frames` is replaced at every stage.
    for (int i = 0; i < FILTER_COUNT; i++) {
        if (!s->filterEnabled[i] || !s->filters[i])
            continue;
        rs2_process_frame(s->filters[i], frames, &e);
        if (failed(e, "process_frame(filter)"))
            return -1;
        frames = rs2_wait_for_frame(s->filterQueues[i], static_cast<unsigned>(timeout_ms), &e);
        if (failed(e, "wait_for_frame(filter)"))
            return -1;
    }

    if (s->align) {
        // rs2_process_frame takes ownership of `frames`; the aligned result comes
        // back from the queue attached to the block at open time.
        rs2_process_frame(s->align, frames, &e);
        if (failed(e, "process_frame(align)"))
            return -1;
        frames = rs2_wait_for_frame(s->queue, static_cast<unsigned>(timeout_ms), &e);
        if (failed(e, "wait_for_frame(align)"))
            return -1;
    }

    // A new frameset invalidates any cloud computed from the previous one.
    releaseFrame(s->points);
    releaseFrame(s->lastDepth);
    releaseFrame(s->lastColor);

    s->lastTimestamp = rs2_get_frame_timestamp(frames, &e);
    if (e)
        rs2_free_error(e);                     // timestamp is informational

    int n = rs2_embedded_frames_count(frames, &e);
    if (failed(e, "embedded_frames_count")) {
        rs2_release_frame(frames);
        return -1;
    }

    int rc = 1;
    for (int i = 0; i < n; i++) {
        rs2_frame* f = rs2_extract_frame(frames, i, &e);
        if (failed(e, "extract_frame")) { rc = -1; continue; }

        const rs2_stream_profile* p = rs2_get_frame_stream_profile(f, &e);
        if (failed(e, "get_frame_stream_profile")) { rs2_release_frame(f); rc = -1; continue; }
        rs2_stream stream = RS2_STREAM_ANY;
        rs2_format format = RS2_FORMAT_ANY;
        int index = 0, uid = 0, framerate = 0;
        rs2_get_stream_profile_data(p, &stream, &format, &index, &uid, &framerate, &e);
        if (failed(e, "get_stream_profile_data")) { rs2_release_frame(f); rc = -1; continue; }

        if (stream == RS2_STREAM_DEPTH) {
            if (depth_dst && !copyFrame(f, depth_dst, size_t(s->depthW) * s->depthH * 2, "read(depth)"))
                rc = -1;
            s->lastDepth = f;                  // retained for rs_points; not released here
            continue;
        } else if (stream == RS2_STREAM_COLOR) {
            if (color_dst && !copyFrame(f, color_dst, size_t(s->colorW) * s->colorH * 3, "read(color)"))
                rc = -1;
            s->lastColor = f;                  // retained to texture the point cloud
            continue;
        } else if (stream == RS2_STREAM_INFRARED && ir_dst) {
            if (!copyFrame(f, ir_dst, size_t(s->irW) * s->irH, "read(infrared)"))
                rc = -1;
        } else if (stream == RS2_STREAM_GYRO || stream == RS2_STREAM_ACCEL) {
            const void* data = rs2_get_frame_data(f, &e);
            if (!failed(e, "read(motion)") && data) {
                float* dst = (stream == RS2_STREAM_GYRO) ? s->gyro : s->accel;
                std::memcpy(dst, data, 3 * sizeof(float));
                if (stream == RS2_STREAM_GYRO) s->hasGyro = true; else s->hasAccel = true;
            }
        }
        rs2_release_frame(f);
    }
    rs2_release_frame(frames);
    return rc;
}

// Convert a raw uint16 depth image to float32 metres. A tight typed loop here
// beats a per-pixel Roxal loop over ~300k pixels by orders of magnitude, and
// this runs once per frame.
extern "C" int rs_depth_to_meters(const void* src, void* dst, int n, double scale)
{
    if (!src || !dst || n < 0) {
        g_lastError = "depth_to_meters: null buffer";
        return -1;
    }
    const uint16_t* in = static_cast<const uint16_t*>(src);
    float* out = static_cast<float*>(dst);
    const float k = static_cast<float>(scale);
    for (int i = 0; i < n; i++)
        out[i] = in[i] * k;                    // 0 stays 0: "no reading at this pixel"
    return 0;
}

//
// calibration (copied at open time; see Session)

// Video-stream intrinsics for RS2_STREAM_DEPTH or RS2_STREAM_COLOR, written into
// a caller float64[12]: width, height, ppx, ppy, fx, fy, model, coeffs[5].
extern "C" int rs_intrinsics(void* h, int stream, double* out12)
{
    Session* s = static_cast<Session*>(h);
    if (!s || !out12) {
        g_lastError = "intrinsics: null session or buffer";
        return -1;
    }
    bool wantColor = (stream == RS2_STREAM_COLOR);
    if (wantColor ? !s->hasColorIntr : !s->hasDepthIntr) {
        g_lastError = "intrinsics: that stream is not enabled";
        return -1;
    }
    const rs2_intrinsics& in = wantColor ? s->colorIntr : s->depthIntr;
    out12[0] = in.width;  out12[1] = in.height;
    out12[2] = in.ppx;    out12[3] = in.ppy;
    out12[4] = in.fx;     out12[5] = in.fy;
    out12[6] = static_cast<double>(in.model);
    for (int i = 0; i < 5; i++)
        out12[7 + i] = in.coeffs[i];
    return 0;
}

// Depth->colour extrinsics, written into a caller float64[12]: rotation
// (column-major 3x3) then translation (metres).
extern "C" int rs_extrinsics(void* h, double* out12)
{
    Session* s = static_cast<Session*>(h);
    if (!s || !out12) {
        g_lastError = "extrinsics: null session or buffer";
        return -1;
    }
    if (!s->hasExtrinsics) {
        g_lastError = "extrinsics: both depth and colour streams must be enabled";
        return -1;
    }
    for (int i = 0; i < 9; i++)
        out12[i] = s->depthToColor.rotation[i];
    for (int i = 0; i < 3; i++)
        out12[9 + i] = s->depthToColor.translation[i];
    return 0;
}

// Deproject a pixel of the depth or colour stream to a metric 3D point, written
// into a caller float64[3]. This defers to the SDK's own rs2_deproject, so every
// distortion model it supports is handled exactly (and a model it cannot invert
// is reported as an error rather than silently approximated).
extern "C" int rs_deproject(void* h, int stream, double x, double y, double depth_m, double* out3)
{
    Session* s = static_cast<Session*>(h);
    if (!s || !out3) {
        g_lastError = "deproject: null session or buffer";
        return -1;
    }
    bool wantColor = (stream == RS2_STREAM_COLOR);
    if (wantColor ? !s->hasColorIntr : !s->hasDepthIntr) {
        g_lastError = "deproject: that stream is not enabled";
        return -1;
    }
    const rs2_intrinsics& in = wantColor ? s->colorIntr : s->depthIntr;
    if (in.model == RS2_DISTORTION_MODIFIED_BROWN_CONRADY) {
        // Forward-distorted: the SDK asserts on this rather than inverting it.
        g_lastError = "deproject: this stream's distortion model cannot be inverted";
        return -1;
    }
    const float pixel[2] = { static_cast<float>(x), static_cast<float>(y) };
    float point[3] = {0, 0, 0};
    rs2_deproject_pixel_to_point(point, &in, pixel, static_cast<float>(depth_m));
    out3[0] = point[0];
    out3[1] = point[1];
    out3[2] = point[2];
    return 0;
}

// Latest motion sample, written into a caller float64[3]: gyro in radians/s
// about each axis, accel in m/s^2 (gravity included). Returns 1 if a sample has
// been seen, 0 if not yet, negative on error.
extern "C" int rs_imu(void* h, int stream, double* out3)
{
    Session* s = static_cast<Session*>(h);
    if (!s || !out3) { g_lastError = "imu: null session or buffer"; return -1; }
    bool wantGyro = (stream == RS2_STREAM_GYRO);
    if (wantGyro ? !s->hasGyro : !s->hasAccel)
        return 0;
    const float* src = wantGyro ? s->gyro : s->accel;
    for (int i = 0; i < 3; i++)
        out3[i] = src[i];
    return 1;
}

//
// point cloud

// Compute a cloud from the frames retained by the last rs_read. Returns the
// number of points (0 if there is no frame yet), or negative on error. When a
// colour frame is present the cloud is textured against it, so rs_points_uv
// gives each point its pixel in the colour image.
extern "C" int rs_points_compute(void* h)
{
    Session* s = static_cast<Session*>(h);
    if (!s) { g_lastError = "points: null session"; return -1; }
    if (!s->lastDepth) {
        g_lastError = "points: no depth frame — call read() first";
        return -1;
    }

    rs2_error* e = nullptr;
    if (!s->pointcloud) {
        s->pointcloud = rs2_create_pointcloud(&e);
        if (failed(e, "create_pointcloud")) return -1;
        s->pcQueue = rs2_create_frame_queue(1, &e);
        if (failed(e, "create_frame_queue(points)")) return -1;
        rs2_start_processing_queue(s->pointcloud, s->pcQueue, &e);
        if (failed(e, "start_processing_queue(points)")) return -1;
    }

    releaseFrame(s->points);

    // Map the cloud against the colour frame — the equivalent of the C++
    // wrapper's pointcloud::map_to(): the block only picks up a texture frame
    // that matches its stream/format/index filter options, so those have to be
    // set before the frame is fed in. Each rs2_process_frame consumes a
    // reference, so hand it its own.
    if (s->lastColor) {
        rs2_options* pcOpts = reinterpret_cast<rs2_options*>(s->pointcloud);
        rs2_set_option(pcOpts, RS2_OPTION_STREAM_FILTER,        float(RS2_STREAM_COLOR), &e);
        if (failed(e, "set_option(stream filter)")) return -1;
        rs2_set_option(pcOpts, RS2_OPTION_STREAM_FORMAT_FILTER, float(RS2_FORMAT_RGB8), &e);
        if (failed(e, "set_option(format filter)")) return -1;
        rs2_set_option(pcOpts, RS2_OPTION_STREAM_INDEX_FILTER,  0.0f, &e);
        if (failed(e, "set_option(index filter)")) return -1;

        rs2_frame_add_ref(s->lastColor, &e);
        if (!failed(e, "frame_add_ref(color)")) {
            rs2_process_frame(s->pointcloud, s->lastColor, &e);
            if (failed(e, "process_frame(pointcloud texture)"))
                return -1;
        }
    }

    rs2_frame_add_ref(s->lastDepth, &e);
    if (failed(e, "frame_add_ref(depth)")) return -1;
    rs2_process_frame(s->pointcloud, s->lastDepth, &e);
    if (failed(e, "process_frame(pointcloud)")) return -1;

    s->points = rs2_wait_for_frame(s->pcQueue, 5000, &e);
    if (failed(e, "wait_for_frame(points)")) return -1;

    int n = rs2_get_frame_points_count(s->points, &e);
    if (failed(e, "get_frame_points_count")) return -1;
    return n;
}

// Copy the computed cloud into a caller float32[n, 3] of metres.
extern "C" int rs_points_copy(void* h, void* out_xyz, int n)
{
    Session* s = static_cast<Session*>(h);
    if (!s || !out_xyz) { g_lastError = "points_copy: null session or buffer"; return -1; }
    if (!s->points) { g_lastError = "points_copy: no cloud — call points_compute first"; return -1; }
    rs2_error* e = nullptr;
    int count = rs2_get_frame_points_count(s->points, &e);
    if (failed(e, "get_frame_points_count")) return -1;
    if (n > count) n = count;
    rs2_vertex* v = rs2_get_frame_vertices(s->points, &e);
    if (failed(e, "get_frame_vertices")) return -1;
    std::memcpy(out_xyz, v, size_t(n) * 3 * sizeof(float));   // rs2_vertex is exactly float[3]
    return n;
}

// Copy each point's texture coordinate into a caller float32[n, 2], as (u, v)
// normalised to [0, 1] against the colour image.
//
// Note the C API declares this as `rs2_pixel*` (int[2]), but that is a mislabel:
// the buffer really holds {float u, v} pairs — the header's own documentation
// says "(u,v) pair within [0,1] range", and the C++ wrapper casts the result to
// its float-based `texture_coordinate`. Reading it as ints yields nonsense.
extern "C" int rs_points_copy_uv(void* h, void* out_uv, int n)
{
    Session* s = static_cast<Session*>(h);
    if (!s || !out_uv) { g_lastError = "points_uv: null session or buffer"; return -1; }
    if (!s->points) { g_lastError = "points_uv: no cloud — call points_compute first"; return -1; }
    if (!s->lastColor) { g_lastError = "points_uv: the cloud has no colour texture (enable the colour stream)"; return -1; }
    rs2_error* e = nullptr;
    int count = rs2_get_frame_points_count(s->points, &e);
    if (failed(e, "get_frame_points_count")) return -1;
    if (n > count) n = count;
    const void* uv = rs2_get_frame_texture_coordinates(s->points, &e);
    if (failed(e, "get_frame_texture_coordinates")) return -1;
    std::memcpy(out_uv, uv, size_t(n) * 2 * sizeof(float));
    return n;
}

//
// depth post-processing filters

namespace {

rs2_processing_block* ensureFilter(Session* s, int filter)
{
    if (filter < 0 || filter >= FILTER_COUNT) {
        g_lastError = "filter: unknown filter id";
        return nullptr;
    }
    if (s->filters[filter])
        return s->filters[filter];

    rs2_error* e = nullptr;
    rs2_processing_block* block = nullptr;
    switch (filter) {
        case FILTER_SPATIAL:      block = rs2_create_spatial_filter_block(&e);      break;
        case FILTER_TEMPORAL:     block = rs2_create_temporal_filter_block(&e);     break;
        case FILTER_HOLE_FILLING: block = rs2_create_hole_filling_filter_block(&e); break;
        default: break;
    }
    if (failed(e, "create_filter_block"))
        return nullptr;
    rs2_frame_queue* q = rs2_create_frame_queue(1, &e);
    if (failed(e, "create_frame_queue(filter)")) {
        rs2_delete_processing_block(block);
        return nullptr;
    }
    rs2_start_processing_queue(block, q, &e);
    if (failed(e, "start_processing_queue(filter)")) {
        rs2_delete_frame_queue(q);
        rs2_delete_processing_block(block);
        return nullptr;
    }
    s->filters[filter] = block;
    s->filterQueues[filter] = q;
    return block;
}

} // namespace

extern "C" int rs_enable_filter(void* h, int filter, int enabled)
{
    Session* s = static_cast<Session*>(h);
    if (!s) { g_lastError = "enable_filter: null session"; return -1; }
    if (enabled && !ensureFilter(s, filter))
        return -1;
    if (filter < 0 || filter >= FILTER_COUNT) {
        g_lastError = "enable_filter: unknown filter id";
        return -1;
    }
    s->filterEnabled[filter] = (enabled != 0);
    return 0;
}

extern "C" int rs_set_filter_option(void* h, int filter, int option, double value)
{
    Session* s = static_cast<Session*>(h);
    if (!s) { g_lastError = "filter_option: null session"; return -1; }
    rs2_processing_block* block = ensureFilter(s, filter);
    if (!block)
        return -1;
    rs2_error* e = nullptr;
    rs2_set_option(reinterpret_cast<rs2_options*>(block), static_cast<rs2_option>(option),
                   static_cast<float>(value), &e);
    if (failed(e, "set_filter_option"))
        return -1;
    return 0;
}

extern "C" int rs_get_filter_option(void* h, int filter, int option, double* out)
{
    Session* s = static_cast<Session*>(h);
    if (!s || !out) { g_lastError = "filter_option: null session or buffer"; return -1; }
    rs2_processing_block* block = ensureFilter(s, filter);
    if (!block)
        return -1;
    rs2_error* e = nullptr;
    float v = rs2_get_option(reinterpret_cast<rs2_options*>(block), static_cast<rs2_option>(option), &e);
    if (failed(e, "get_filter_option"))
        return -1;
    *out = v;
    return 0;
}

//
// sensor options (emitter power, exposure, presets, ...)

namespace {

rs2_options* sensorOptions(Session* s, int sensor)
{
    rs2_sensor* sen = (sensor == 1) ? s->colorSensor : s->depthSensor;
    if (!sen) {
        g_lastError = "option: that sensor is not present on this device";
        return nullptr;
    }
    return reinterpret_cast<rs2_options*>(sen);
}

} // namespace

// 1 if supported, 0 if not, negative on error.
extern "C" int rs_supports_option(void* h, int sensor, int option)
{
    Session* s = static_cast<Session*>(h);
    if (!s) { g_lastError = "supports_option: null session"; return -1; }
    rs2_options* opts = sensorOptions(s, sensor);
    if (!opts)
        return -1;
    rs2_error* e = nullptr;
    int sup = rs2_supports_option(opts, static_cast<rs2_option>(option), &e);
    if (failed(e, "supports_option"))
        return -1;
    return sup ? 1 : 0;
}

extern "C" int rs_get_option(void* h, int sensor, int option, double* out)
{
    Session* s = static_cast<Session*>(h);
    if (!s || !out) { g_lastError = "get_option: null session or buffer"; return -1; }
    rs2_options* opts = sensorOptions(s, sensor);
    if (!opts)
        return -1;
    rs2_error* e = nullptr;
    float v = rs2_get_option(opts, static_cast<rs2_option>(option), &e);
    if (failed(e, "get_option"))
        return -1;
    *out = v;
    return 0;
}

extern "C" int rs_set_option(void* h, int sensor, int option, double value)
{
    Session* s = static_cast<Session*>(h);
    if (!s) { g_lastError = "set_option: null session"; return -1; }
    rs2_options* opts = sensorOptions(s, sensor);
    if (!opts)
        return -1;
    rs2_error* e = nullptr;
    rs2_set_option(opts, static_cast<rs2_option>(option), static_cast<float>(value), &e);
    if (failed(e, "set_option"))
        return -1;
    return 0;
}

// Option range, written into a caller float64[4]: min, max, step, default.
extern "C" int rs_option_range(void* h, int sensor, int option, double* out4)
{
    Session* s = static_cast<Session*>(h);
    if (!s || !out4) { g_lastError = "option_range: null session or buffer"; return -1; }
    rs2_options* opts = sensorOptions(s, sensor);
    if (!opts)
        return -1;
    float mn = 0, mx = 0, step = 0, def = 0;
    rs2_error* e = nullptr;
    rs2_get_option_range(opts, static_cast<rs2_option>(option), &mn, &mx, &step, &def, &e);
    if (failed(e, "get_option_range"))
        return -1;
    out4[0] = mn; out4[1] = mx; out4[2] = step; out4[3] = def;
    return 0;
}
