// cvx_shim: flat C ABI over OpenCV 5 for the Roxal opencv module (loaded via sys.loadlib).
//
// Conventions:
//  - Images cross the boundary as contiguous uint8 [H, W, C] buffers in RGB channel
//    order (Roxal's image convention); conversion to/from OpenCV's native BGR happens
//    here, at the edge.
//  - Functions taking caller-allocated output buffers wrap them as borrowed cv::Mat
//    headers (zero copy). The caller must size them correctly; a post-call check
//    catches accidental reallocation.
//  - int-returning functions: 0 = ok, negative = error (message via cvx_last_error).
//    Pointer-returning functions: NULL = error.
//  - Opaque handles (cvx_imread result, VideoCapture) are freed by the paired
//    *_release function, which Roxal registers as a @cfunc free= finalizer.

#include <opencv2/core.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/geometry.hpp>   // OpenCV 5: boundingRect/contourArea/getRotationMatrix2D live here
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include <cstring>
#include <map>
#include <mutex>
#include <string>

// The Roxal-side declarations (init.rox, including its generated section) are
// written against this exact OpenCV feature surface; review and regenerate
// before bumping the vendored version.
static_assert(CV_VERSION_MAJOR == 5 && CV_VERSION_MINOR == 0,
              "cvx_shim is written against OpenCV 5.0 — update init.rox/generate.py before building against a different version");

static thread_local std::string g_lastError;

// Roxal reports failures via cvx_last_error; keep OpenCV's own console chatter down.
static const bool g_quietLogs = [] {
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);
    return true;
}();

#define CVX_TRY try {
#define CVX_CATCH(errret) \
    } catch (const std::exception& e) { g_lastError = e.what(); return errret; } \
      catch (...) { g_lastError = "unknown error"; return errret; }

static cv::Mat borrow(const void* data, int h, int w, int c)
{
    return cv::Mat(h, w, CV_8UC(c), const_cast<void*>(data));
}

extern "C" {

const char* cvx_last_error(void) { return g_lastError.c_str(); }

const char* cvx_version(void) { return CV_VERSION; }

//
// Mat handles (used where OpenCV determines the output size, e.g. image decode)

void cvx_mat_release(void* mat)
{
    delete static_cast<cv::Mat*>(mat);
}

int cvx_mat_rows(void* mat)     { return static_cast<cv::Mat*>(mat)->rows; }
int cvx_mat_cols(void* mat)     { return static_cast<cv::Mat*>(mat)->cols; }
int cvx_mat_channels(void* mat) { return static_cast<cv::Mat*>(mat)->channels(); }

// Copy a handle's pixels into a caller-allocated contiguous buffer.
int cvx_mat_copy_to(void* mat, void* dst, int64_t dstBytes)
{
    CVX_TRY
    cv::Mat* m = static_cast<cv::Mat*>(mat);
    int64_t need = int64_t(m->total()) * m->elemSize();
    if (dstBytes != need) {
        g_lastError = "destination buffer size mismatch";
        return -1;
    }
    cv::Mat view(m->rows, m->cols, m->type(), dst);
    m->copyTo(view);
    return 0;
    CVX_CATCH(-1)
}

//
// imgcodecs

// Decode an image file; result is an RGB (or RGBA/gray, per requested channels) Mat handle.
// channels: 0 = as-is (color files -> 3), 1 = grayscale, 3 = RGB, 4 = RGBA.
void* cvx_imread(const char* path, int channels)
{
    CVX_TRY
    int flags = (channels == 1) ? cv::IMREAD_GRAYSCALE
              : (channels == 4) ? cv::IMREAD_UNCHANGED
                                : cv::IMREAD_COLOR;
    cv::Mat img = cv::imread(path, flags);
    if (img.empty()) {
        g_lastError = std::string("could not read image '") + path + "'";
        return nullptr;
    }
    if (img.depth() != CV_8U)
        img.convertTo(img, CV_8U);
    if (img.channels() == 3)
        cv::cvtColor(img, img, cv::COLOR_BGR2RGB);
    else if (img.channels() == 4)
        cv::cvtColor(img, img, cv::COLOR_BGRA2RGBA);
    if (channels == 4 && img.channels() == 3)
        cv::cvtColor(img, img, cv::COLOR_RGB2RGBA);
    return new cv::Mat(std::move(img));
    CVX_CATCH(nullptr)
}

int cvx_imwrite(const char* path, const void* data, int h, int w, int c)
{
    CVX_TRY
    cv::Mat img = borrow(data, h, w, c);
    cv::Mat out;
    if (c == 3)      cv::cvtColor(img, out, cv::COLOR_RGB2BGR);
    else if (c == 4) cv::cvtColor(img, out, cv::COLOR_RGBA2BGRA);
    else             out = img;
    if (!cv::imwrite(path, out)) {
        g_lastError = std::string("could not write image '") + path + "'";
        return -1;
    }
    return 0;
    CVX_CATCH(-1)
}

//
// imgproc (tensor-direct: caller allocates correctly-sized outputs)

int cvx_resize(const void* src, int sh, int sw, int c,
               void* dst, int dh, int dw, int interpolation)
{
    CVX_TRY
    cv::Mat s = borrow(src, sh, sw, c);
    cv::Mat d = borrow(dst, dh, dw, c);
    void* dp = d.data;
    cv::resize(s, d, cv::Size(dw, dh), 0, 0, interpolation);
    if (d.data != dp) { g_lastError = "resize: output buffer size mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

int cvx_cvt_color(const void* src, int h, int w, int sc,
                  void* dst, int dc, int code)
{
    CVX_TRY
    cv::Mat s = borrow(src, h, w, sc);
    cv::Mat d = borrow(dst, h, w, dc);
    void* dp = d.data;
    cv::cvtColor(s, d, code);
    if (d.data != dp) { g_lastError = "cvt_color: output channel mismatch for code"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

int cvx_gaussian_blur(const void* src, int h, int w, int c,
                      void* dst, int ksize, double sigma)
{
    CVX_TRY
    cv::Mat s = borrow(src, h, w, c);
    cv::Mat d = borrow(dst, h, w, c);
    void* dp = d.data;
    cv::GaussianBlur(s, d, cv::Size(ksize, ksize), sigma);
    if (d.data != dp) { g_lastError = "gaussian_blur: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

// src/dst are single-channel [H, W, 1]
int cvx_canny(const void* src, int h, int w,
              void* dst, double threshold1, double threshold2)
{
    CVX_TRY
    cv::Mat s = borrow(src, h, w, 1);
    cv::Mat d = borrow(dst, h, w, 1);
    void* dp = d.data;
    cv::Canny(s, d, threshold1, threshold2);
    if (d.data != dp) { g_lastError = "canny: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

// single-channel threshold; returns the computed threshold (interesting for OTSU),
// or a negative value on error
double cvx_threshold(const void* src, int h, int w,
                     void* dst, double thresh, double maxval, int type)
{
    CVX_TRY
    cv::Mat s = borrow(src, h, w, 1);
    cv::Mat d = borrow(dst, h, w, 1);
    void* dp = d.data;
    double r = cv::threshold(s, d, thresh, maxval, type);
    if (d.data != dp) { g_lastError = "threshold: output buffer mismatch"; return -1.0; }
    return r;
    CVX_CATCH(-1.0)
}

//
// videoio

void* cvx_cap_open(int index)
{
    CVX_TRY
    auto* cap = new cv::VideoCapture(index);
    if (!cap->isOpened()) {
        delete cap;
        g_lastError = "could not open camera " + std::to_string(index);
        return nullptr;
    }
    return cap;
    CVX_CATCH(nullptr)
}

void* cvx_cap_open_file(const char* path)
{
    CVX_TRY
    auto* cap = new cv::VideoCapture(path);
    if (!cap->isOpened()) {
        delete cap;
        g_lastError = std::string("could not open video '") + path + "'";
        return nullptr;
    }
    return cap;
    CVX_CATCH(nullptr)
}

void cvx_cap_release(void* cap)
{
    delete static_cast<cv::VideoCapture*>(cap);
}

double cvx_cap_get(void* cap, int prop)
{
    return static_cast<cv::VideoCapture*>(cap)->get(prop);
}

int cvx_cap_set(void* cap, int prop, double value)
{
    return static_cast<cv::VideoCapture*>(cap)->set(prop, value) ? 0 : -1;
}

// Read the next frame into a caller-allocated RGB uint8 [h, w, 3] buffer.
// Returns 1 on success, 0 on end-of-stream, negative on error.
int cvx_cap_read(void* cap, void* dst, int h, int w)
{
    CVX_TRY
    cv::Mat frame;
    if (!static_cast<cv::VideoCapture*>(cap)->read(frame) || frame.empty())
        return 0;
    if (frame.rows != h || frame.cols != w) {
        g_lastError = "frame size does not match buffer";
        return -1;
    }
    cv::Mat d = borrow(dst, h, w, 3);
    if (frame.channels() == 3)      cv::cvtColor(frame, d, cv::COLOR_BGR2RGB);
    else if (frame.channels() == 1) cv::cvtColor(frame, d, cv::COLOR_GRAY2RGB);
    else                            cv::cvtColor(frame, d, cv::COLOR_BGRA2RGB);
    return 1;
    CVX_CATCH(-1)
}

} // extern "C"

//
// drawing (in-place on the caller's RGB tensor buffer; color passed as RGB)

static cv::Scalar rgbScalar(double r, double g, double b, int c)
{
    return (c == 4) ? cv::Scalar(r, g, b, 255) : cv::Scalar(r, g, b);
}

extern "C" {

int cvx_line(void* img, int h, int w, int c, int x1, int y1, int x2, int y2,
             double r, double g, double b, int thickness)
{
    CVX_TRY
    cv::Mat m = borrow(img, h, w, c);
    cv::line(m, cv::Point(x1, y1), cv::Point(x2, y2), rgbScalar(r, g, b, c), thickness, cv::LINE_AA);
    return 0;
    CVX_CATCH(-1)
}

int cvx_rectangle(void* img, int h, int w, int c, int x, int y, int rw, int rh,
                  double r, double g, double b, int thickness)
{
    CVX_TRY
    cv::Mat m = borrow(img, h, w, c);
    cv::rectangle(m, cv::Rect(x, y, rw, rh), rgbScalar(r, g, b, c), thickness, cv::LINE_AA);
    return 0;
    CVX_CATCH(-1)
}

int cvx_circle(void* img, int h, int w, int c, int cx, int cy, int radius,
               double r, double g, double b, int thickness)
{
    CVX_TRY
    cv::Mat m = borrow(img, h, w, c);
    cv::circle(m, cv::Point(cx, cy), radius, rgbScalar(r, g, b, c), thickness, cv::LINE_AA);
    return 0;
    CVX_CATCH(-1)
}

int cvx_put_text(void* img, int h, int w, int c, const char* text, int x, int y,
                 int font, double scale, double r, double g, double b, int thickness)
{
    CVX_TRY
    cv::Mat m = borrow(img, h, w, c);
    cv::putText(m, text, cv::Point(x, y), font, scale, rgbScalar(r, g, b, c), thickness, cv::LINE_AA);
    return 0;
    CVX_CATCH(-1)
}

//
// geometric transforms

int cvx_warp_affine(const void* src, int sh, int sw, int c,
                    void* dst, int dh, int dw, const double* m6, int interp)
{
    CVX_TRY
    cv::Mat s = borrow(src, sh, sw, c);
    cv::Mat d = borrow(dst, dh, dw, c);
    void* dp = d.data;
    cv::Mat m(2, 3, CV_64F, const_cast<double*>(m6));
    cv::warpAffine(s, d, m, cv::Size(dw, dh), interp);
    if (d.data != dp) { g_lastError = "warp_affine: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

int cvx_warp_perspective(const void* src, int sh, int sw, int c,
                         void* dst, int dh, int dw, const double* m9, int interp)
{
    CVX_TRY
    cv::Mat s = borrow(src, sh, sw, c);
    cv::Mat d = borrow(dst, dh, dw, c);
    void* dp = d.data;
    cv::Mat m(3, 3, CV_64F, const_cast<double*>(m9));
    cv::warpPerspective(s, d, m, cv::Size(dw, dh), interp);
    if (d.data != dp) { g_lastError = "warp_perspective: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

int cvx_rotation_matrix_2d(double cx, double cy, double angle, double scale, double* out6)
{
    CVX_TRY
    cv::Mat m = cv::getRotationMatrix2D(cv::Point2f(float(cx), float(cy)), angle, scale);
    memcpy(out6, m.ptr<double>(), 6 * sizeof(double));
    return 0;
    CVX_CATCH(-1)
}

// 90-degree-step rotation; caller sizes dst (dims swap for 90/270)
int cvx_rotate(const void* src, int sh, int sw, int c,
               void* dst, int dh, int dw, int code)
{
    CVX_TRY
    cv::Mat s = borrow(src, sh, sw, c);
    cv::Mat d = borrow(dst, dh, dw, c);
    void* dp = d.data;
    cv::rotate(s, d, code);
    if (d.data != dp) { g_lastError = "rotate: output buffer size mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

//
// contours (input: single-channel binary image)

struct CvxContours { std::vector<std::vector<cv::Point>> cs; };

void* cvx_find_contours(const void* src, int h, int w, int mode, int method)
{
    CVX_TRY
    cv::Mat s = borrow(src, h, w, 1);
    auto* cc = new CvxContours;
    cv::findContours(s, cc->cs, mode, method);
    return cc;
    CVX_CATCH(nullptr)
}

void cvx_contours_release(void* cc) { delete static_cast<CvxContours*>(cc); }

int cvx_contours_count(void* cc) { return int(static_cast<CvxContours*>(cc)->cs.size()); }

int cvx_contour_len(void* cc, int i)
{
    auto* c = static_cast<CvxContours*>(cc);
    if (i < 0 || i >= int(c->cs.size())) return -1;
    return int(c->cs[i].size());
}

// copy contour i into an int32 [n, 2] buffer as x, y rows
int cvx_contour_copy(void* cc, int i, int32_t* dst)
{
    CVX_TRY
    auto* c = static_cast<CvxContours*>(cc);
    if (i < 0 || i >= int(c->cs.size())) { g_lastError = "contour index out of range"; return -1; }
    for (const cv::Point& p : c->cs[i]) {
        *dst++ = p.x;
        *dst++ = p.y;
    }
    return 0;
    CVX_CATCH(-1)
}

double cvx_contour_area(const int32_t* pts, int n)
{
    CVX_TRY
    std::vector<cv::Point> v(n);
    for (int i = 0; i < n; i++) v[i] = cv::Point(pts[2*i], pts[2*i+1]);
    return cv::contourArea(v);
    CVX_CATCH(-1.0)
}

int cvx_bounding_rect(const int32_t* pts, int n, int32_t* out4)
{
    CVX_TRY
    std::vector<cv::Point> v(n);
    for (int i = 0; i < n; i++) v[i] = cv::Point(pts[2*i], pts[2*i+1]);
    cv::Rect r = cv::boundingRect(v);
    out4[0] = r.x; out4[1] = r.y; out4[2] = r.width; out4[3] = r.height;
    return 0;
    CVX_CATCH(-1)
}

//
// video: deterministic close + VideoWriter

// release the camera/file now (LED off, file closed); the handle itself is
// freed later by the GC finalizer
void cvx_cap_close(void* cap) { static_cast<cv::VideoCapture*>(cap)->release(); }

struct CvxWriter { cv::VideoWriter w; int width, height; };

void* cvx_writer_open(const char* path, const char* fourcc, double fps, int w, int h)
{
    CVX_TRY
    if (strlen(fourcc) != 4) { g_lastError = "codec must be a 4-character code, e.g. 'mp4v'"; return nullptr; }
    int fcc = cv::VideoWriter::fourcc(fourcc[0], fourcc[1], fourcc[2], fourcc[3]);
    auto* wr = new CvxWriter{cv::VideoWriter(path, fcc, fps, cv::Size(w, h)), w, h};
    if (!wr->w.isOpened()) {
        delete wr;
        g_lastError = std::string("could not open video writer for '") + path + "'";
        return nullptr;
    }
    return wr;
    CVX_CATCH(nullptr)
}

int cvx_writer_write(void* wrp, const void* data, int h, int w)
{
    CVX_TRY
    auto* wr = static_cast<CvxWriter*>(wrp);
    if (h != wr->height || w != wr->width) {
        g_lastError = "frame size does not match VideoWriter size";
        return -1;
    }
    if (!wr->w.isOpened()) { g_lastError = "write after close"; return -1; }
    cv::Mat rgb = borrow(data, h, w, 3);
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    wr->w.write(bgr);
    return 0;
    CVX_CATCH(-1)
}

// flush and close the output file now; memory freed later by the GC finalizer
void cvx_writer_close(void* wrp) { static_cast<CvxWriter*>(wrp)->w.release(); }

void cvx_writer_release(void* wrp) { delete static_cast<CvxWriter*>(wrp); }

} // extern "C"


//
// ============================== Tier 2 ==============================

#include <opencv2/objdetect.hpp>   // OpenCV 5: findChessboardCorners lives here
#include <opencv2/objdetect/aruco_detector.hpp>
#include <opencv2/calib.hpp>
#include <opencv2/features.hpp>
#include <opencv2/stereo.hpp>

// shared post-decode normalization: 8-bit + RGB/RGBA channel order
static cv::Mat* cvxFinishDecoded(cv::Mat img, int channels)
{
    if (img.depth() != CV_8U)
        img.convertTo(img, CV_8U);
    if (img.channels() == 3)
        cv::cvtColor(img, img, cv::COLOR_BGR2RGB);
    else if (img.channels() == 4)
        cv::cvtColor(img, img, cv::COLOR_BGRA2RGBA);
    if (channels == 4 && img.channels() == 3)
        cv::cvtColor(img, img, cv::COLOR_RGB2RGBA);
    return new cv::Mat(std::move(img));
}

// RGB/RGBA/gray borrow -> single-channel gray for detectors
static cv::Mat cvxToGray(const void* img, int h, int w, int c)
{
    cv::Mat m = borrow(img, h, w, c);
    if (c == 1)
        return m;
    cv::Mat gray;
    cv::cvtColor(m, gray, c == 4 ? cv::COLOR_RGBA2GRAY : cv::COLOR_RGB2GRAY);
    return gray;
}

extern "C" {

//
// in-memory image codecs

// encode an RGB image; returns a Mat handle holding the byte stream (1 x N)
void* cvx_imencode(const char* ext, const void* data, int h, int w, int c, int quality)
{
    CVX_TRY
    cv::Mat img = borrow(data, h, w, c);
    cv::Mat bgr;
    if (c == 3)      cv::cvtColor(img, bgr, cv::COLOR_RGB2BGR);
    else if (c == 4) cv::cvtColor(img, bgr, cv::COLOR_RGBA2BGRA);
    else             bgr = img;
    std::vector<uchar> buf;
    std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, quality, cv::IMWRITE_WEBP_QUALITY, quality};
    if (!cv::imencode(ext, bgr, buf, params)) {
        g_lastError = std::string("could not encode image as '") + ext + "'";
        return nullptr;
    }
    cv::Mat* out = new cv::Mat(1, int(buf.size()), CV_8U);
    memcpy(out->data, buf.data(), buf.size());
    return out;
    CVX_CATCH(nullptr)
}

void* cvx_imdecode(const void* buf, int64_t nbytes, int channels)
{
    CVX_TRY
    cv::Mat data(1, int(nbytes), CV_8U, const_cast<void*>(buf));
    int flags = (channels == 1) ? cv::IMREAD_GRAYSCALE
              : (channels == 4) ? cv::IMREAD_UNCHANGED
                                : cv::IMREAD_COLOR;
    cv::Mat img = cv::imdecode(data, flags);
    if (img.empty()) {
        g_lastError = "could not decode image data";
        return nullptr;
    }
    return cvxFinishDecoded(std::move(img), channels);
    CVX_CATCH(nullptr)
}

//
// ArUco markers

struct CvxArucoResult {
    std::vector<int> ids;
    std::vector<std::vector<cv::Point2f>> corners;
};

// render marker `id` of the dictionary into a caller-allocated [side, side, 1] buffer
int cvx_aruco_generate(int dict_id, int id, int side, void* dst)
{
    CVX_TRY
    cv::aruco::Dictionary dict = cv::aruco::getPredefinedDictionary(dict_id);
    cv::Mat img;
    dict.generateImageMarker(id, side, img);
    cv::Mat d(side, side, CV_8U, dst);
    img.copyTo(d);
    return 0;
    CVX_CATCH(-1)
}

// Detector instances are cached per dictionary here in the shim (construction
// builds marker tables).  A C++-side cache keeps the Roxal module stateless:
// callers may run on any VM thread (main, actors, the dataflow engine), where
// mutating Roxal module state would be forbidden.  detectMarkers() is const.
static cv::aruco::ArucoDetector& cvxArucoDetector(int dict_id)
{
    static std::mutex mu;
    static std::map<int, cv::aruco::ArucoDetector> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(dict_id);
    if (it == cache.end())
        it = cache.emplace(dict_id,
                           cv::aruco::ArucoDetector(cv::aruco::getPredefinedDictionary(dict_id))).first;
    return it->second;
}

void* cvx_aruco_detect(const void* img, int h, int w, int c, int dict_id)
{
    CVX_TRY
    cv::Mat gray = cvxToGray(img, h, w, c);
    auto* r = new CvxArucoResult;
    cvxArucoDetector(dict_id).detectMarkers(gray, r->corners, r->ids);
    return r;
    CVX_CATCH(nullptr)
}

void cvx_aruco_release(void* r) { delete static_cast<CvxArucoResult*>(r); }

int cvx_aruco_count(void* r) { return int(static_cast<CvxArucoResult*>(r)->ids.size()); }

// copy marker i's corners into a float32 [4, 2] buffer; returns the marker id
int cvx_aruco_get(void* rp, int i, float* corners8)
{
    CVX_TRY
    auto* r = static_cast<CvxArucoResult*>(rp);
    if (i < 0 || i >= int(r->ids.size())) { g_lastError = "marker index out of range"; return -1; }
    for (int j = 0; j < 4; j++) {
        corners8[2*j]   = r->corners[i][j].x;
        corners8[2*j+1] = r->corners[i][j].y;
    }
    return r->ids[i];
    CVX_CATCH(-1)
}

//
// calibration / pose

static cv::Mat cvxDistMat(const double* dist, int ndist)
{
    return ndist > 0 ? cv::Mat(1, ndist, CV_64F, const_cast<double*>(dist)) : cv::Mat();
}

int cvx_solve_pnp(const float* obj, const float* imgp, int n, const double* cam9,
                  const double* dist, int ndist, double* rvec3, double* tvec3)
{
    CVX_TRY
    cv::Mat op(n, 3, CV_32F, const_cast<float*>(obj));
    cv::Mat ip(n, 2, CV_32F, const_cast<float*>(imgp));
    cv::Mat cam(3, 3, CV_64F, const_cast<double*>(cam9));
    cv::Mat rv, tv;
    if (!cv::solvePnP(op, ip, cam, cvxDistMat(dist, ndist), rv, tv)) {
        g_lastError = "solvePnP failed";
        return -1;
    }
    rv.convertTo(rv, CV_64F);
    tv.convertTo(tv, CV_64F);
    memcpy(rvec3, rv.ptr<double>(), 3 * sizeof(double));
    memcpy(tvec3, tv.ptr<double>(), 3 * sizeof(double));
    return 0;
    CVX_CATCH(-1)
}

int cvx_project_points(const float* obj, int n, const double* rvec3, const double* tvec3,
                       const double* cam9, const double* dist, int ndist, float* out)
{
    CVX_TRY
    cv::Mat op(n, 3, CV_32F, const_cast<float*>(obj));
    cv::Mat rv(3, 1, CV_64F, const_cast<double*>(rvec3));
    cv::Mat tv(3, 1, CV_64F, const_cast<double*>(tvec3));
    cv::Mat cam(3, 3, CV_64F, const_cast<double*>(cam9));
    std::vector<cv::Point2f> result;
    cv::projectPoints(op, rv, tv, cam, cvxDistMat(dist, ndist), result);
    for (int i = 0; i < n; i++) {
        out[2*i]   = result[i].x;
        out[2*i+1] = result[i].y;
    }
    return 0;
    CVX_CATCH(-1)
}

// finds inner chessboard corners; returns 1 found / 0 not found / negative error
int cvx_find_chessboard(const void* img, int h, int w, int c, int cols, int rows, float* out)
{
    CVX_TRY
    cv::Mat gray = cvxToGray(img, h, w, c);
    std::vector<cv::Point2f> corners;
    if (!cv::findChessboardCorners(gray, cv::Size(cols, rows), corners))
        return 0;
    cv::cornerSubPix(gray, corners, cv::Size(5, 5), cv::Size(-1, -1),
                     cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 30, 0.01));
    for (size_t i = 0; i < corners.size(); i++) {
        out[2*i]   = corners[i].x;
        out[2*i+1] = corners[i].y;
    }
    return 1;
    CVX_CATCH(-1)
}

// obj: concatenated [total, 3] f32; imgp: [total, 2] f32; counts: per-view point counts.
// Writes 3x3 camera matrix and 5 distortion coefficients; returns reprojection error.
double cvx_calibrate_camera(const float* obj, const float* imgp, const int32_t* counts,
                            int nviews, int img_w, int img_h, double* cam9, double* dist5)
{
    CVX_TRY
    std::vector<std::vector<cv::Point3f>> op(nviews);
    std::vector<std::vector<cv::Point2f>> ip(nviews);
    int off = 0;
    for (int v = 0; v < nviews; v++) {
        int n = counts[v];
        op[v].assign(reinterpret_cast<const cv::Point3f*>(obj) + off,
                     reinterpret_cast<const cv::Point3f*>(obj) + off + n);
        ip[v].assign(reinterpret_cast<const cv::Point2f*>(imgp) + off,
                     reinterpret_cast<const cv::Point2f*>(imgp) + off + n);
        off += n;
    }
    cv::Mat cam, dist;
    double err = cv::calibrateCamera(op, ip, cv::Size(img_w, img_h), cam, dist,
                                     cv::noArray(), cv::noArray());
    memcpy(cam9, cam.ptr<double>(), 9 * sizeof(double));
    for (int i = 0; i < 5; i++)
        dist5[i] = i < dist.cols * dist.rows ? dist.ptr<double>()[i] : 0.0;
    return err;
    CVX_CATCH(-1.0)
}

int cvx_undistort(const void* src, int h, int w, int c, void* dst,
                  const double* cam9, const double* dist, int ndist)
{
    CVX_TRY
    cv::Mat s = borrow(src, h, w, c);
    cv::Mat d = borrow(dst, h, w, c);
    void* dp = d.data;
    cv::Mat cam(3, 3, CV_64F, const_cast<double*>(cam9));
    cv::undistort(s, d, cam, cvxDistMat(dist, ndist));
    if (d.data != dp) { g_lastError = "undistort: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

// g2b16/t2c16: n row-major 4x4 gripper->base and target->camera transforms.
// Writes the camera->gripper 4x4 into out16.
int cvx_calibrate_hand_eye(const double* g2b16, const double* t2c16, int n,
                           int method, double* out16)
{
    CVX_TRY
    std::vector<cv::Mat> Rg, tg, Rt, tt;
    for (int i = 0; i < n; i++) {
        cv::Mat g(4, 4, CV_64F, const_cast<double*>(g2b16 + 16 * i));
        cv::Mat t(4, 4, CV_64F, const_cast<double*>(t2c16 + 16 * i));
        Rg.push_back(g(cv::Rect(0, 0, 3, 3)).clone());
        tg.push_back(g(cv::Rect(3, 0, 1, 3)).clone());
        Rt.push_back(t(cv::Rect(0, 0, 3, 3)).clone());
        tt.push_back(t(cv::Rect(3, 0, 1, 3)).clone());
    }
    cv::Mat R, t;
    cv::calibrateHandEye(Rg, tg, Rt, tt, R, t, cv::HandEyeCalibrationMethod(method));
    cv::Mat out(4, 4, CV_64F, out16);
    out.setTo(0.0);
    R.copyTo(out(cv::Rect(0, 0, 3, 3)));
    t.copyTo(out(cv::Rect(3, 0, 1, 3)));
    out.at<double>(3, 3) = 1.0;
    return 0;
    CVX_CATCH(-1)
}

//
// features (ORB) + matching

struct CvxFeatures {
    std::vector<cv::KeyPoint> kps;
    cv::Mat desc;
};

void* cvx_orb_detect(const void* img, int h, int w, int c, int nfeatures)
{
    CVX_TRY
    cv::Mat gray = cvxToGray(img, h, w, c);
    auto orb = cv::ORB::create(nfeatures);
    auto* f = new CvxFeatures;
    orb->detectAndCompute(gray, cv::noArray(), f->kps, f->desc);
    return f;
    CVX_CATCH(nullptr)
}

void cvx_features_release(void* f) { delete static_cast<CvxFeatures*>(f); }

int cvx_features_count(void* f) { return int(static_cast<CvxFeatures*>(f)->kps.size()); }

int cvx_features_desc_cols(void* f) { return static_cast<CvxFeatures*>(f)->desc.cols; }

// float32 [N, 4]: x, y, size, angle
int cvx_features_keypoints(void* fp, float* out)
{
    CVX_TRY
    auto* f = static_cast<CvxFeatures*>(fp);
    for (size_t i = 0; i < f->kps.size(); i++) {
        out[4*i]   = f->kps[i].pt.x;
        out[4*i+1] = f->kps[i].pt.y;
        out[4*i+2] = f->kps[i].size;
        out[4*i+3] = f->kps[i].angle;
    }
    return 0;
    CVX_CATCH(-1)
}

// 0 = uint8 descriptors (ORB), 1 = float32 (ALIKED)
int cvx_features_desc_type(void* f)
{
    return static_cast<CvxFeatures*>(f)->desc.depth() == CV_32F ? 1 : 0;
}

int cvx_features_descriptors(void* fp, void* out)
{
    CVX_TRY
    auto* f = static_cast<CvxFeatures*>(fp);
    if (!f->desc.isContinuous()) { g_lastError = "descriptors not contiguous"; return -1; }
    memcpy(out, f->desc.data, size_t(f->desc.rows) * f->desc.cols * f->desc.elemSize());
    return 0;
    CVX_CATCH(-1)
}

// cross-checked Hamming brute-force matching of two binary descriptor sets.
// pairs: int32 [n1, 2] (queryIdx, trainIdx); dists: float32 [n1]. Returns match count.
int cvx_match_hamming(const uint8_t* d1, int n1, const uint8_t* d2, int n2, int dsize,
                      int32_t* pairs, float* dists)
{
    CVX_TRY
    cv::Mat m1(n1, dsize, CV_8U, const_cast<uint8_t*>(d1));
    cv::Mat m2(n2, dsize, CV_8U, const_cast<uint8_t*>(d2));
    cv::BFMatcher matcher(cv::NORM_HAMMING, true);
    std::vector<cv::DMatch> ms;
    matcher.match(m1, m2, ms);
    for (size_t i = 0; i < ms.size(); i++) {
        pairs[2*i]   = ms[i].queryIdx;
        pairs[2*i+1] = ms[i].trainIdx;
        dists[i]     = ms[i].distance;
    }
    return int(ms.size());
    CVX_CATCH(-1)
}

//
// stereo depth

// semi-global block matching on a rectified gray pair; out: float32 [h, w] disparity
// in pixels (invalid pixels < min_disparity)
int cvx_stereo_sgbm(const void* left, const void* right, int h, int w,
                    int min_disp, int num_disp, int block_size, float* out)
{
    CVX_TRY
    cv::Mat l(h, w, CV_8U, const_cast<void*>(left));
    cv::Mat r(h, w, CV_8U, const_cast<void*>(right));
    auto sgbm = cv::StereoSGBM::create(min_disp, num_disp, block_size);
    sgbm->setP1(8 * block_size * block_size);
    sgbm->setP2(32 * block_size * block_size);
    cv::Mat disp16;
    sgbm->compute(l, r, disp16);
    cv::Mat dispF(h, w, CV_32F, out);
    void* dp = dispF.data;
    disp16.convertTo(dispF, CV_32F, 1.0 / 16.0);
    if (dispF.data != dp) { g_lastError = "disparity: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

} // extern "C"


//
// ============================== Tier 3 ==============================

#include <opencv2/objdetect/charuco_detector.hpp>

extern "C" {

//
// ChArUco boards (chessboard + ArUco: subpixel corners with marker identity)

void* cvx_charuco_board_new(int sx, int sy, double square_len, double marker_len, int dict_id)
{
    CVX_TRY
    return new cv::aruco::CharucoBoard(cv::Size(sx, sy), float(square_len), float(marker_len),
                                       cv::aruco::getPredefinedDictionary(dict_id));
    CVX_CATCH(nullptr)
}

void cvx_charuco_board_release(void* b) { delete static_cast<cv::aruco::CharucoBoard*>(b); }

// render the board into a caller-allocated gray [h, w, 1] buffer
int cvx_charuco_board_image(void* b, void* dst, int h, int w, int margin)
{
    CVX_TRY
    cv::Mat img;
    static_cast<cv::aruco::CharucoBoard*>(b)->generateImage(cv::Size(w, h), img, margin);
    cv::Mat d(h, w, CV_8U, dst);
    img.copyTo(d);
    return 0;
    CVX_CATCH(-1)
}

struct CvxCharucoResult {
    std::vector<cv::Point2f> corners;
    std::vector<int> ids;
};

void* cvx_charuco_detect(void* b, const void* img, int h, int w, int c)
{
    CVX_TRY
    cv::Mat gray = cvxToGray(img, h, w, c);
    cv::aruco::CharucoDetector det(*static_cast<cv::aruco::CharucoBoard*>(b));
    auto* r = new CvxCharucoResult;
    det.detectBoard(gray, r->corners, r->ids);
    return r;
    CVX_CATCH(nullptr)
}

void cvx_charuco_release(void* r) { delete static_cast<CvxCharucoResult*>(r); }

int cvx_charuco_count(void* r) { return int(static_cast<CvxCharucoResult*>(r)->corners.size()); }

// copy detected corners (float32 [N, 2]) and ids (int32 [N])
int cvx_charuco_copy(void* rp, float* corners, int32_t* ids)
{
    CVX_TRY
    auto* r = static_cast<CvxCharucoResult*>(rp);
    for (size_t i = 0; i < r->corners.size(); i++) {
        corners[2*i]   = r->corners[i].x;
        corners[2*i+1] = r->corners[i].y;
        ids[i]         = r->ids[i];
    }
    return 0;
    CVX_CATCH(-1)
}

// matched calibration points for a detection: board-frame object points (float32
// [N, 3]) and their image points (float32 [N, 2]), via the board's corner ids
int cvx_charuco_match(void* bp, void* rp, float* obj, float* imgp)
{
    CVX_TRY
    auto* board = static_cast<cv::aruco::CharucoBoard*>(bp);
    auto* r = static_cast<CvxCharucoResult*>(rp);
    std::vector<cv::Point3f> all = board->getChessboardCorners();
    for (size_t i = 0; i < r->ids.size(); i++) {
        int id = r->ids[i];
        if (id < 0 || id >= int(all.size())) { g_lastError = "charuco id out of range"; return -1; }
        obj[3*i]   = all[id].x;
        obj[3*i+1] = all[id].y;
        obj[3*i+2] = all[id].z;
        imgp[2*i]   = r->corners[i].x;
        imgp[2*i+1] = r->corners[i].y;
    }
    return 0;
    CVX_CATCH(-1)
}

//
// stereo calibration + rectification

// intrinsics held fixed (calibrate each camera first); writes the right-camera
// pose relative to the left (R 3x3, T 3) and returns the reprojection error
double cvx_stereo_calibrate(const float* obj, const float* ipl, const float* ipr,
                            const int32_t* counts, int nviews, int w, int h,
                            const double* cam1_9, const double* dist1, int nd1,
                            const double* cam2_9, const double* dist2, int nd2,
                            double* R9, double* T3)
{
    CVX_TRY
    std::vector<std::vector<cv::Point3f>> op(nviews);
    std::vector<std::vector<cv::Point2f>> il(nviews), ir(nviews);
    int off = 0;
    for (int v = 0; v < nviews; v++) {
        int n = counts[v];
        op[v].assign(reinterpret_cast<const cv::Point3f*>(obj) + off,
                     reinterpret_cast<const cv::Point3f*>(obj) + off + n);
        il[v].assign(reinterpret_cast<const cv::Point2f*>(ipl) + off,
                     reinterpret_cast<const cv::Point2f*>(ipl) + off + n);
        ir[v].assign(reinterpret_cast<const cv::Point2f*>(ipr) + off,
                     reinterpret_cast<const cv::Point2f*>(ipr) + off + n);
        off += n;
    }
    cv::Mat cam1 = cv::Mat(3, 3, CV_64F, const_cast<double*>(cam1_9)).clone();
    cv::Mat cam2 = cv::Mat(3, 3, CV_64F, const_cast<double*>(cam2_9)).clone();
    cv::Mat d1 = cvxDistMat(dist1, nd1).clone();
    cv::Mat d2 = cvxDistMat(dist2, nd2).clone();
    cv::Mat R, T, E, F;
    double err = cv::stereoCalibrate(op, il, ir, cam1, d1, cam2, d2, cv::Size(w, h),
                                     R, T, E, F, cv::CALIB_FIX_INTRINSIC);
    memcpy(R9, R.ptr<double>(), 9 * sizeof(double));
    memcpy(T3, T.ptr<double>(), 3 * sizeof(double));
    return err;
    CVX_CATCH(-1.0)
}

int cvx_stereo_rectify(const double* cam1_9, const double* dist1, int nd1,
                       const double* cam2_9, const double* dist2, int nd2,
                       int w, int h, const double* R9, const double* T3,
                       double* R1_9, double* R2_9, double* P1_12, double* P2_12, double* Q16)
{
    CVX_TRY
    cv::Mat cam1(3, 3, CV_64F, const_cast<double*>(cam1_9));
    cv::Mat cam2(3, 3, CV_64F, const_cast<double*>(cam2_9));
    cv::Mat R(3, 3, CV_64F, const_cast<double*>(R9));
    cv::Mat T(3, 1, CV_64F, const_cast<double*>(T3));
    cv::Mat R1, R2, P1, P2, Q;
    cv::stereoRectify(cam1, cvxDistMat(dist1, nd1), cam2, cvxDistMat(dist2, nd2),
                      cv::Size(w, h), R, T, R1, R2, P1, P2, Q);
    memcpy(R1_9, R1.ptr<double>(), 9 * sizeof(double));
    memcpy(R2_9, R2.ptr<double>(), 9 * sizeof(double));
    memcpy(P1_12, P1.ptr<double>(), 12 * sizeof(double));
    memcpy(P2_12, P2.ptr<double>(), 12 * sizeof(double));
    memcpy(Q16, Q.ptr<double>(), 16 * sizeof(double));
    return 0;
    CVX_CATCH(-1)
}

// undistort + rectify one camera's image using its stereoRectify outputs
int cvx_rectify_remap(const void* src, int h, int w, int c, void* dst,
                      const double* cam9, const double* dist, int ndist,
                      const double* R9, const double* P12)
{
    CVX_TRY
    cv::Mat s = borrow(src, h, w, c);
    cv::Mat d = borrow(dst, h, w, c);
    void* dp = d.data;
    cv::Mat cam(3, 3, CV_64F, const_cast<double*>(cam9));
    cv::Mat R(3, 3, CV_64F, const_cast<double*>(R9));
    cv::Mat P(3, 4, CV_64F, const_cast<double*>(P12));
    cv::Mat map1, map2;
    cv::initUndistortRectifyMap(cam, cvxDistMat(dist, ndist), R, P, cv::Size(w, h),
                                CV_32FC1, map1, map2);
    cv::remap(s, d, map1, map2, cv::INTER_LINEAR);
    if (d.data != dp) { g_lastError = "rectify: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

//
// planar geometry estimation

int cvx_find_homography(const float* srcp, const float* dstp, int n,
                        int method, double ransac_thresh, double* h9)
{
    CVX_TRY
    cv::Mat s(n, 2, CV_32F, const_cast<float*>(srcp));
    cv::Mat d(n, 2, CV_32F, const_cast<float*>(dstp));
    cv::Mat H = cv::findHomography(s, d, method, ransac_thresh);
    if (H.empty()) { g_lastError = "findHomography failed (degenerate points?)"; return -1; }
    memcpy(h9, H.ptr<double>(), 9 * sizeof(double));
    return 0;
    CVX_CATCH(-1)
}

int cvx_estimate_affine_2d(const float* srcp, const float* dstp, int n, double* m6)
{
    CVX_TRY
    cv::Mat s(n, 2, CV_32F, const_cast<float*>(srcp));
    cv::Mat d(n, 2, CV_32F, const_cast<float*>(dstp));
    cv::Mat M = cv::estimateAffine2D(s, d);
    if (M.empty()) { g_lastError = "estimateAffine2D failed (degenerate points?)"; return -1; }
    memcpy(m6, M.ptr<double>(), 6 * sizeof(double));
    return 0;
    CVX_CATCH(-1)
}

} // extern "C"


//
// ============================== Tier 4 (DNN task wrappers) ==============================

#include <opencv2/objdetect/face.hpp>
#include <opencv2/video/tracking.hpp>

// most OpenCV DNN task models expect BGR input
static cv::Mat cvxToBgr(const void* img, int h, int w, int c)
{
    cv::Mat m = borrow(img, h, w, c);
    cv::Mat bgr;
    if (c == 3)      cv::cvtColor(m, bgr, cv::COLOR_RGB2BGR);
    else if (c == 4) cv::cvtColor(m, bgr, cv::COLOR_RGBA2BGR);
    else             cv::cvtColor(m, bgr, cv::COLOR_GRAY2BGR);
    return bgr;
}

extern "C" {

// Convert an RGB image into a caller-allocated NCHW float32 blob [1, 3, oh, ow]
// for DNN input: resize, scale, then per-channel (v*scale - mean) / std.
int cvx_to_blob(const void* src, int h, int w, int c, float* out, int oh, int ow,
                double scale, double mr, double mg, double mb,
                double sr, double sg, double sb)
{
    CVX_TRY
    if (c != 3) { g_lastError = "to_blob: expects an RGB [H, W, 3] image"; return -1; }
    cv::Mat s = borrow(src, h, w, c);
    cv::Mat resized;
    cv::resize(s, resized, cv::Size(ow, oh), 0, 0, cv::INTER_LINEAR);
    cv::Mat f;
    resized.convertTo(f, CV_32FC3, scale);
    cv::subtract(f, cv::Scalar(mr, mg, mb), f);
    cv::divide(f, cv::Scalar(sr, sg, sb), f);
    cv::Mat planes[3] = {cv::Mat(oh, ow, CV_32F, out),
                         cv::Mat(oh, ow, CV_32F, out + size_t(oh) * ow),
                         cv::Mat(oh, ow, CV_32F, out + 2 * size_t(oh) * ow)};
    void* p0 = planes[0].data;
    cv::split(f, planes);
    if (planes[0].data != p0) { g_lastError = "to_blob: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

//
// face detection (YuNet)

struct CvxFaceDetector { cv::Ptr<cv::FaceDetectorYN> d; };

void* cvx_face_new(const char* model, double score_thresh, double nms_thresh)
{
    CVX_TRY
    auto d = cv::FaceDetectorYN::create(model, "", cv::Size(320, 320),
                                        float(score_thresh), float(nms_thresh));
    return new CvxFaceDetector{d};
    CVX_CATCH(nullptr)
}

void cvx_face_release(void* f) { delete static_cast<CvxFaceDetector*>(f); }

// out: float32 [max_faces, 15] rows of x,y,w,h, 5 landmark x,y pairs, score.
// Returns the number of faces written.
int cvx_face_detect(void* fp, const void* img, int h, int w, int c,
                    float* out, int max_faces)
{
    CVX_TRY
    auto* f = static_cast<CvxFaceDetector*>(fp);
    cv::Mat bgr = cvxToBgr(img, h, w, c);
    f->d->setInputSize(cv::Size(w, h));
    cv::Mat faces;
    f->d->detect(bgr, faces);
    int n = std::min(faces.rows, max_faces);
    for (int i = 0; i < n; i++)
        memcpy(out + 15 * i, faces.ptr<float>(i), 15 * sizeof(float));
    return n;
    CVX_CATCH(-1)
}

//
// learned features: ALIKED detector + LightGlue matcher

struct CvxAliked { cv::Ptr<cv::ALIKED> a; };

void* cvx_aliked_new(const char* model)
{
    CVX_TRY
    return new CvxAliked{cv::ALIKED::create(model)};
    CVX_CATCH(nullptr)
}

void cvx_aliked_release(void* a) { delete static_cast<CvxAliked*>(a); }

void* cvx_aliked_detect(void* ap, const void* img, int h, int w, int c)
{
    CVX_TRY
    auto* a = static_cast<CvxAliked*>(ap);
    cv::Mat bgr = cvxToBgr(img, h, w, c);
    auto* f = new CvxFeatures;
    a->a->detectAndCompute(bgr, cv::noArray(), f->kps, f->desc);
    return f;
    CVX_CATCH(nullptr)
}

struct CvxLightGlue { cv::Ptr<cv::LightGlueMatcher> m; };

void* cvx_lightglue_new(const char* model, double score_thresh)
{
    CVX_TRY
    return new CvxLightGlue{cv::LightGlueMatcher::create(model, float(score_thresh))};
    CVX_CATCH(nullptr)
}

void cvx_lightglue_release(void* m) { delete static_cast<CvxLightGlue*>(m); }

// match two ALIKED feature sets given as raw data: kps float32 [n, 4]
// (x, y, size, angle), desc float32 [n, dcols]; image sizes give LightGlue its
// spatial context. pairs: int32 [na, 2]; dists: float32 [na]. Returns count.
int cvx_lightglue_match(void* mp,
                        const float* kpsa, const float* desca, int na,
                        const float* kpsb, const float* descb, int nb, int dcols,
                        int wa, int ha, int wb, int hb,
                        int32_t* pairs, float* dists)
{
    CVX_TRY
    auto* m = static_cast<CvxLightGlue*>(mp);
    cv::Mat qk(na, 2, CV_32F);
    cv::Mat tk(nb, 2, CV_32F);
    for (int i = 0; i < na; i++) {
        qk.at<float>(i, 0) = kpsa[4*i];
        qk.at<float>(i, 1) = kpsa[4*i+1];
    }
    for (int i = 0; i < nb; i++) {
        tk.at<float>(i, 0) = kpsb[4*i];
        tk.at<float>(i, 1) = kpsb[4*i+1];
    }
    cv::Mat da(na, dcols, CV_32F, const_cast<float*>(desca));
    cv::Mat db(nb, dcols, CV_32F, const_cast<float*>(descb));
    m->m->setPairInfo(qk, tk, cv::Size(wa, ha), cv::Size(wb, hb));
    std::vector<cv::DMatch> ms;
    m->m->match(da, db, ms);
    for (size_t i = 0; i < ms.size(); i++) {
        pairs[2*i]   = ms[i].queryIdx;
        pairs[2*i+1] = ms[i].trainIdx;
        dists[i]     = ms[i].distance;
    }
    return int(ms.size());
    CVX_CATCH(-1)
}

//
// single-object tracking (VIT)

struct CvxTracker { cv::Ptr<cv::TrackerVit> t; };

void* cvx_tracker_new(const char* model)
{
    CVX_TRY
    cv::TrackerVit::Params p;
    p.net = model;
    return new CvxTracker{cv::TrackerVit::create(p)};
    CVX_CATCH(nullptr)
}

void cvx_tracker_release(void* t) { delete static_cast<CvxTracker*>(t); }

int cvx_tracker_init(void* tp, const void* img, int h, int w, int c,
                     int x, int y, int rw, int rh)
{
    CVX_TRY
    static_cast<CvxTracker*>(tp)->t->init(cvxToBgr(img, h, w, c), cv::Rect(x, y, rw, rh));
    return 0;
    CVX_CATCH(-1)
}

// returns 1 tracking / 0 lost; writes [x, y, w, h] and the confidence score
int cvx_tracker_update(void* tp, const void* img, int h, int w, int c,
                       int32_t* rect4, double* score)
{
    CVX_TRY
    auto* t = static_cast<CvxTracker*>(tp);
    cv::Rect box;
    bool ok = t->t->update(cvxToBgr(img, h, w, c), box);
    rect4[0] = box.x; rect4[1] = box.y; rect4[2] = box.width; rect4[3] = box.height;
    *score = t->t->getTrackingScore();
    return ok ? 1 : 0;
    CVX_CATCH(-1)
}

} // extern "C"


//
// ============================== Tier 5 (analysis kit, flow, fisheye) ==============================

#include <opencv2/video/background_segm.hpp>

extern "C" {

// mask of pixels where every channel lies within [lo, hi] (bounds per channel)
int cvx_in_range(const void* src, int h, int w, int c, void* dst,
                 double l0, double l1, double l2, double l3,
                 double u0, double u1, double u2, double u3)
{
    CVX_TRY
    cv::Mat s = borrow(src, h, w, c);
    cv::Mat d(h, w, CV_8U, dst);
    void* dp = d.data;
    cv::inRange(s, cv::Scalar(l0, l1, l2, l3), cv::Scalar(u0, u1, u2, u3), d);
    if (d.data != dp) { g_lastError = "in_range: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

static std::vector<cv::Point> cvxPoints(const int32_t* pts, int n)
{
    std::vector<cv::Point> v(n);
    for (int i = 0; i < n; i++) v[i] = cv::Point(pts[2*i], pts[2*i+1]);
    return v;
}

int cvx_draw_contour(void* img, int h, int w, int c, const int32_t* pts, int n,
                     double r, double g, double b, int thickness)
{
    CVX_TRY
    cv::Mat m = borrow(img, h, w, c);
    std::vector<std::vector<cv::Point>> cs{cvxPoints(pts, n)};
    cv::drawContours(m, cs, 0, rgbScalar(r, g, b, c), thickness, cv::LINE_AA);
    return 0;
    CVX_CATCH(-1)
}

// out capacity n*2; returns hull point count
int cvx_convex_hull(const int32_t* pts, int n, int32_t* out)
{
    CVX_TRY
    std::vector<cv::Point> hull;
    cv::convexHull(cvxPoints(pts, n), hull);
    for (size_t i = 0; i < hull.size(); i++) {
        out[2*i]   = hull[i].x;
        out[2*i+1] = hull[i].y;
    }
    return int(hull.size());
    CVX_CATCH(-1)
}

// out capacity n*2; returns simplified point count
int cvx_approx_poly(const int32_t* pts, int n, double epsilon, int closed, int32_t* out)
{
    CVX_TRY
    std::vector<cv::Point> approx;
    cv::approxPolyDP(cvxPoints(pts, n), approx, epsilon, closed != 0);
    for (size_t i = 0; i < approx.size(); i++) {
        out[2*i]   = approx[i].x;
        out[2*i+1] = approx[i].y;
    }
    return int(approx.size());
    CVX_CATCH(-1)
}

// out5: cx, cy, w, h, angle_degrees
int cvx_min_area_rect(const int32_t* pts, int n, double* out5)
{
    CVX_TRY
    cv::RotatedRect rr = cv::minAreaRect(cvxPoints(pts, n));
    out5[0] = rr.center.x; out5[1] = rr.center.y;
    out5[2] = rr.size.width; out5[3] = rr.size.height;
    out5[4] = rr.angle;
    return 0;
    CVX_CATCH(-1)
}

// corners of a rotated rect: float32 [4, 2]
int cvx_box_points(double cx, double cy, double w, double h, double angle, float* out8)
{
    CVX_TRY
    cv::Mat pts;
    cv::boxPoints(cv::RotatedRect(cv::Point2f(float(cx), float(cy)),
                                  cv::Size2f(float(w), float(h)), float(angle)), pts);
    memcpy(out8, pts.ptr<float>(), 8 * sizeof(float));
    return 0;
    CVX_CATCH(-1)
}

//
// connected components (on a binary mask)

struct CvxCC { cv::Mat labels, stats, centroids; int n; };

void* cvx_cc_new(const void* src, int h, int w)
{
    CVX_TRY
    cv::Mat s(h, w, CV_8U, const_cast<void*>(src));
    auto* cc = new CvxCC;
    cc->n = cv::connectedComponentsWithStats(s, cc->labels, cc->stats, cc->centroids);
    return cc;
    CVX_CATCH(nullptr)
}

void cvx_cc_release(void* cc) { delete static_cast<CvxCC*>(cc); }

int cvx_cc_count(void* cc) { return static_cast<CvxCC*>(cc)->n; }

// stats: int32 [n, 5] (left, top, width, height, area); centroids: float64 [n, 2]
int cvx_cc_copy(void* ccp, int32_t* stats, double* centroids)
{
    CVX_TRY
    auto* cc = static_cast<CvxCC*>(ccp);
    memcpy(stats, cc->stats.ptr<int32_t>(), size_t(cc->n) * 5 * sizeof(int32_t));
    memcpy(centroids, cc->centroids.ptr<double>(), size_t(cc->n) * 2 * sizeof(double));
    return 0;
    CVX_CATCH(-1)
}

//
// template matching: best-match location into loc2, returns the match value

int cvx_match_template(const void* img, int h, int w, int c,
                       const void* tmpl, int th, int tw, int method,
                       int32_t* loc2, double* score_out)
{
    CVX_TRY
    cv::Mat m = borrow(img, h, w, c);
    cv::Mat t = borrow(tmpl, th, tw, c);
    cv::Mat result;
    cv::matchTemplate(m, t, result, method);
    double mn, mx;
    cv::Point mnLoc, mxLoc;
    cv::minMaxLoc(result, &mn, &mx, &mnLoc, &mxLoc);
    bool useMin = (method == cv::TM_SQDIFF || method == cv::TM_SQDIFF_NORMED);
    cv::Point best = useMin ? mnLoc : mxLoc;
    loc2[0] = best.x; loc2[1] = best.y;
    *score_out = useMin ? mn : mx;
    return 0;
    CVX_CATCH(-1)
}

//
// QR codes

struct CvxQr { std::vector<std::string> texts; std::vector<cv::Point2f> pts; };

void* cvx_qr_detect(const void* img, int h, int w, int c)
{
    CVX_TRY
    cv::Mat gray = cvxToGray(img, h, w, c);
    cv::QRCodeDetector det;
    auto* r = new CvxQr;
    std::vector<std::string> texts;
    std::vector<cv::Point2f> points;
    if (det.detectAndDecodeMulti(gray, texts, points)) {
        r->texts = std::move(texts);
        r->pts = std::move(points);
    }
    return r;
    CVX_CATCH(nullptr)
}

void cvx_qr_release(void* r) { delete static_cast<CvxQr*>(r); }

int cvx_qr_count(void* r) { return int(static_cast<CvxQr*>(r)->texts.size()); }

const char* cvx_qr_text(void* rp, int i)
{
    auto* r = static_cast<CvxQr*>(rp);
    if (i < 0 || i >= int(r->texts.size())) return nullptr;
    return r->texts[i].c_str();
}

int cvx_qr_corners(void* rp, int i, float* out8)
{
    CVX_TRY
    auto* r = static_cast<CvxQr*>(rp);
    if (i < 0 || i >= int(r->texts.size())) { g_lastError = "qr index out of range"; return -1; }
    for (int j = 0; j < 4; j++) {
        out8[2*j]   = r->pts[4*i + j].x;
        out8[2*j+1] = r->pts[4*i + j].y;
    }
    return 0;
    CVX_CATCH(-1)
}

// render `text` as a QR code into a gray [side, side, 1] buffer
int cvx_qr_encode(const char* text, int side, void* dst)
{
    CVX_TRY
    cv::Mat qr;
    cv::QRCodeEncoder::create()->encode(text, qr);
    cv::Mat d(side, side, CV_8U, dst);
    void* dp = d.data;
    cv::resize(qr, d, cv::Size(side, side), 0, 0, cv::INTER_NEAREST);
    if (d.data != dp) { g_lastError = "qr_encode: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

//
// stereo: disparity -> 3D points via the Q matrix

// mark_missing: pixels at the minimal disparity (SGBM's invalid marker) get a
// very large Z instead of a bogus depth
int cvx_reproject_3d(const float* disp, int h, int w, const double* q16, float* out,
                     int mark_missing)
{
    CVX_TRY
    cv::Mat d(h, w, CV_32F, const_cast<float*>(disp));
    cv::Mat o(h, w, CV_32FC3, out);
    void* op = o.data;
    cv::Mat Q(4, 4, CV_64F, const_cast<double*>(q16));
    cv::reprojectImageTo3D(d, o, Q, mark_missing != 0);
    if (o.data != op) { g_lastError = "reproject_3d: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

//
// cached rectification maps (avoids rebuilding undistort maps every frame)

struct CvxRectifyMap { cv::Mat map1, map2; };

void* cvx_rectify_map_new(const double* cam9, const double* dist, int ndist,
                          const double* R9, const double* P12, int w, int h)
{
    CVX_TRY
    cv::Mat cam(3, 3, CV_64F, const_cast<double*>(cam9));
    cv::Mat R(3, 3, CV_64F, const_cast<double*>(R9));
    cv::Mat P(3, 4, CV_64F, const_cast<double*>(P12));
    auto* m = new CvxRectifyMap;
    cv::initUndistortRectifyMap(cam, cvxDistMat(dist, ndist), R, P, cv::Size(w, h),
                                CV_32FC1, m->map1, m->map2);
    return m;
    CVX_CATCH(nullptr)
}

void cvx_rectify_map_release(void* m) { delete static_cast<CvxRectifyMap*>(m); }

int cvx_rectify_map_apply(void* mp, const void* src, int h, int w, int c, void* dst)
{
    CVX_TRY
    auto* m = static_cast<CvxRectifyMap*>(mp);
    cv::Mat s = borrow(src, h, w, c);
    cv::Mat d = borrow(dst, h, w, c);
    void* dp = d.data;
    cv::remap(s, d, m->map1, m->map2, cv::INTER_LINEAR);
    if (d.data != dp) { g_lastError = "RectifyMap.apply: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

//
// background subtraction (MOG2)

struct CvxMog2 { cv::Ptr<cv::BackgroundSubtractorMOG2> bs; };

void* cvx_mog2_new(int history, double var_threshold, int shadows)
{
    CVX_TRY
    return new CvxMog2{cv::createBackgroundSubtractorMOG2(history, var_threshold, shadows != 0)};
    CVX_CATCH(nullptr)
}

void cvx_mog2_release(void* m) { delete static_cast<CvxMog2*>(m); }

int cvx_mog2_apply(void* mp, const void* img, int h, int w, int c,
                   void* mask, double learning_rate)
{
    CVX_TRY
    cv::Mat m = borrow(img, h, w, c);
    cv::Mat d(h, w, CV_8U, mask);
    void* dp = d.data;
    static_cast<CvxMog2*>(mp)->bs->apply(m, d, learning_rate);
    if (d.data != dp) { g_lastError = "BackgroundSubtractor.apply: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

//
// optical flow (on grayscale images)

// out capacity max_corners*2; returns corner count
int cvx_good_features(const void* gray, int h, int w, int max_corners,
                      double quality, double min_dist, float* out)
{
    CVX_TRY
    cv::Mat g(h, w, CV_8U, const_cast<void*>(gray));
    std::vector<cv::Point2f> corners;
    cv::goodFeaturesToTrack(g, corners, max_corners, quality, min_dist);
    for (size_t i = 0; i < corners.size(); i++) {
        out[2*i]   = corners[i].x;
        out[2*i+1] = corners[i].y;
    }
    return int(corners.size());
    CVX_CATCH(-1)
}

// sparse Lucas-Kanade: track pts from prev to next; out float32 [n, 2], status uint8 [n]
int cvx_flow_lk(const void* prev, const void* next, int h, int w,
                const float* pts, int n, float* out, uint8_t* status)
{
    CVX_TRY
    cv::Mat p(h, w, CV_8U, const_cast<void*>(prev));
    cv::Mat nx(h, w, CV_8U, const_cast<void*>(next));
    std::vector<cv::Point2f> v0(n), v1;
    for (int i = 0; i < n; i++) v0[i] = cv::Point2f(pts[2*i], pts[2*i+1]);
    std::vector<uchar> st;
    std::vector<float> err;
    cv::calcOpticalFlowPyrLK(p, nx, v0, v1, st, err);
    for (int i = 0; i < n; i++) {
        out[2*i]   = v1[i].x;
        out[2*i+1] = v1[i].y;
        status[i]  = st[i];
    }
    return 0;
    CVX_CATCH(-1)
}

// dense Farneback flow: out float32 [h, w, 2] (dx, dy per pixel)
int cvx_flow_dense(const void* prev, const void* next, int h, int w, float* out)
{
    CVX_TRY
    cv::Mat p(h, w, CV_8U, const_cast<void*>(prev));
    cv::Mat nx(h, w, CV_8U, const_cast<void*>(next));
    cv::Mat flow(h, w, CV_32FC2, out);
    void* fp = flow.data;
    cv::calcOpticalFlowFarneback(p, nx, flow, 0.5, 3, 15, 3, 5, 1.2, cv::OPTFLOW_USE_INITIAL_FLOW * 0);
    if (flow.data != fp) { g_lastError = "flow_dense: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

// contrast-limited adaptive histogram equalization (grayscale)
int cvx_clahe(const void* src, int h, int w, void* dst, double clip, int tiles)
{
    CVX_TRY
    cv::Mat s(h, w, CV_8U, const_cast<void*>(src));
    cv::Mat d(h, w, CV_8U, dst);
    void* dp = d.data;
    cv::createCLAHE(clip, cv::Size(tiles, tiles))->apply(s, d);
    if (d.data != dp) { g_lastError = "clahe: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

//
// fisheye camera model

double cvx_fisheye_calibrate(const float* obj, const float* imgp, const int32_t* counts,
                             int nviews, int w, int h, double* cam9, double* dist4)
{
    CVX_TRY
    std::vector<std::vector<cv::Point3f>> op(nviews);
    std::vector<std::vector<cv::Point2f>> ip(nviews);
    int off = 0;
    for (int v = 0; v < nviews; v++) {
        int n = counts[v];
        op[v].assign(reinterpret_cast<const cv::Point3f*>(obj) + off,
                     reinterpret_cast<const cv::Point3f*>(obj) + off + n);
        ip[v].assign(reinterpret_cast<const cv::Point2f*>(imgp) + off,
                     reinterpret_cast<const cv::Point2f*>(imgp) + off + n);
        off += n;
    }
    cv::Mat K, D;
    int flags = cv::fisheye::CALIB_RECOMPUTE_EXTRINSIC | cv::fisheye::CALIB_FIX_SKEW;
    double err = cv::fisheye::calibrate(op, ip, cv::Size(w, h), K, D,
                                        cv::noArray(), cv::noArray(), flags);
    memcpy(cam9, K.ptr<double>(), 9 * sizeof(double));
    memcpy(dist4, D.ptr<double>(), 4 * sizeof(double));
    return err;
    CVX_CATCH(-1.0)
}

int cvx_fisheye_undistort(const void* src, int h, int w, int c, void* dst,
                          const double* cam9, const double* dist4)
{
    CVX_TRY
    cv::Mat s = borrow(src, h, w, c);
    cv::Mat d = borrow(dst, h, w, c);
    void* dp = d.data;
    cv::Mat K(3, 3, CV_64F, const_cast<double*>(cam9));
    cv::Mat D(1, 4, CV_64F, const_cast<double*>(dist4));
    cv::fisheye::undistortImage(s, d, K, D, K);
    if (d.data != dp) { g_lastError = "undistort_fisheye: output buffer mismatch"; return -1; }
    return 0;
    CVX_CATCH(-1)
}

int cvx_fisheye_project(const float* obj, int n, const double* rvec3, const double* tvec3,
                        const double* cam9, const double* dist4, float* out)
{
    CVX_TRY
    std::vector<cv::Point3f> op(reinterpret_cast<const cv::Point3f*>(obj),
                                reinterpret_cast<const cv::Point3f*>(obj) + n);
    cv::Mat rv(3, 1, CV_64F, const_cast<double*>(rvec3));
    cv::Mat tv(3, 1, CV_64F, const_cast<double*>(tvec3));
    cv::Mat K(3, 3, CV_64F, const_cast<double*>(cam9));
    cv::Mat D(1, 4, CV_64F, const_cast<double*>(dist4));
    std::vector<cv::Point2f> result;
    cv::fisheye::projectPoints(op, result, rv, tv, K, D);
    for (int i = 0; i < n; i++) {
        out[2*i]   = result[i].x;
        out[2*i+1] = result[i].y;
    }
    return 0;
    CVX_CATCH(-1)
}

} // extern "C"

#include "cvx_gen.inc"
