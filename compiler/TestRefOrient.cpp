/* TestRefOrient.cpp — Cross-validate Roxal's orient type (ObjOrient, VM
   property getters, VM methods) against the independent TestRefOrient
   reference implementation (opensim-derived).

   Invoked via: _runtests('orient')
   Returns results in the standard _runtests tuple format.

   Each test constructs ObjOrient values via Value::orientVal() and exercises
   the actual VM getter/method code paths, comparing their results against
   TestRefOrient which uses entirely independent conversion math.
*/

#include "TestRefOrient.h"
#include "Value.h"
#include "Object.h"
#include "VM.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cmath>
#include <string>
#include <vector>
#include <tuple>
#include <sstream>

namespace roxal {

using Result = std::tuple<std::string, bool, std::string>;
using Results = std::vector<Result>;

static constexpr double DEG = M_PI / 180.0;
static constexpr double EPS = 1e-10;
static constexpr double EULER_EPS = 1e-8;

// ---- helpers to extract numeric values from Roxal Value objects ----

static bool approx(double a, double b, double eps = EPS) {
    return std::abs(a - b) < eps;
}

// Extract radians from a sys.quantity Value with angle dimension
static double quantityToRadians(const Value& v) {
    if (!isObjectInstance(v))
        throw std::runtime_error("expected quantity, got non-instance");
    return asObjectInstance(v)->getProperty("_v").asReal();
}

// Extract 3 doubles from a Roxal vector Value
static std::array<double,3> vec3FromValue(const Value& v) {
    auto* vec = asVector(v);
    return {vec->vec()[0], vec->vec()[1], vec->vec()[2]};
}

// Extract 4 doubles from a Roxal vector Value [x,y,z,w]
static std::array<double,4> vec4FromValue(const Value& v) {
    auto* vec = asVector(v);
    return {vec->vec()[0], vec->vec()[1], vec->vec()[2], vec->vec()[3]};
}

// Extract 3x3 row-major doubles from a Roxal matrix Value
static std::array<double,9> mat3FromValue(const Value& v) {
    auto* mat = asMatrix(v);
    std::array<double,9> m;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            m[r*3+c] = mat->mat()(r,c);
    return m;
}

// Compare arrays element-wise
static bool arr3Equal(const std::array<double,3>& a, const std::array<double,3>& b, double eps = EPS) {
    return approx(a[0],b[0],eps) && approx(a[1],b[1],eps) && approx(a[2],b[2],eps);
}

static bool arr9Equal(const std::array<double,9>& a, const std::array<double,9>& b, double eps = EPS) {
    for (int i = 0; i < 9; i++)
        if (!approx(a[i], b[i], eps)) return false;
    return true;
}

// Compare quaternions accounting for double-cover (q ≡ -q)
static bool quatArrEqual(const std::array<double,4>& a, const std::array<double,4>& b, double eps = EPS) {
    double d1 = 0, d2 = 0;
    for (int i = 0; i < 4; i++) {
        d1 += (a[i]-b[i])*(a[i]-b[i]);
        d2 += (a[i]+b[i])*(a[i]+b[i]);
    }
    return std::min(d1, d2) < eps*eps*4;
}

// Build Eigen quaternion from RPY the same way Roxal's orient constructor does
static Eigen::Quaterniond eigenFromRPY(double r, double p, double y) {
    return Eigen::AngleAxisd(y, Eigen::Vector3d::UnitZ())
         * Eigen::AngleAxisd(p, Eigen::Vector3d::UnitY())
         * Eigen::AngleAxisd(r, Eigen::Vector3d::UnitX());
}

// ---- Test functions ----
// Each constructs orient Values through the Roxal API and compares
// the property getter / method results against TestRefOrient.

static Result test_identity() {
    Value ov = Value::orientVal();  // identity orient
    VM& vm = VM::instance();
    TestRefOrient ref;

    // .quat
    auto roxQuat = vec4FromValue(vm.orient_quat_getter(ov));
    auto refQuat = ref.getQuat();
    if (!quatArrEqual(roxQuat, refQuat))
        return {"identity", false, "quat mismatch"};

    // .mat
    auto roxMat = mat3FromValue(vm.orient_mat_getter(ov));
    auto refMat = ref.getMatrix();
    if (!arr9Equal(roxMat, refMat))
        return {"identity", false, "matrix mismatch"};

    // .r, .p, .y
    double rr = quantityToRadians(vm.orient_r_getter(ov));
    double rp = quantityToRadians(vm.orient_p_getter(ov));
    double ry = quantityToRadians(vm.orient_y_getter(ov));
    if (!approx(rr,0) || !approx(rp,0) || !approx(ry,0))
        return {"identity", false, "RPY mismatch"};

    return {"identity", true, "ok"};
}

static Result test_rpy_getters() {
    VM& vm = VM::instance();
    struct Case { double r,p,y; const char* name; };
    Case cases[] = {
        {0, 0, 90*DEG, "yaw90"},
        {90*DEG, 0, 0, "roll90"},
        {0, 45*DEG, 0, "pitch45"},
        {30*DEG, 45*DEG, 60*DEG, "r30p45y60"},
        {-20*DEG, 10*DEG, 170*DEG, "negative"},
        {45*DEG, -30*DEG, 120*DEG, "mixed"},
    };
    for (auto& c : cases) {
        auto ref = TestRefOrient::fromRPY(c.r, c.p, c.y);
        auto refRPY = ref.getRPY();

        Value ov = Value::orientVal(eigenFromRPY(c.r, c.p, c.y));

        double rr = quantityToRadians(vm.orient_r_getter(ov));
        double rp = quantityToRadians(vm.orient_p_getter(ov));
        double ry = quantityToRadians(vm.orient_y_getter(ov));
        if (!approx(rr, refRPY[0], EULER_EPS) ||
            !approx(rp, refRPY[1], EULER_EPS) ||
            !approx(ry, refRPY[2], EULER_EPS))
            return {"rpy_getters", false, std::string("failed at ") + c.name};
    }
    return {"rpy_getters", true, "6 cases ok"};
}

static Result test_rpy_list_getter() {
    VM& vm = VM::instance();
    double r = 30*DEG, p = 45*DEG, y = 60*DEG;
    auto ref = TestRefOrient::fromRPY(r, p, y);
    auto refRPY = ref.getRPY();

    Value ov = Value::orientVal(eigenFromRPY(r, p, y));
    Value rpyList = vm.orient_rpy_getter(ov);
    if (!isList(rpyList) || asList(rpyList)->length() != 3)
        return {"rpy_list_getter", false, "not a 3-element list"};

    double rr = quantityToRadians(asList(rpyList)->getElement(0));
    double rp = quantityToRadians(asList(rpyList)->getElement(1));
    double ry = quantityToRadians(asList(rpyList)->getElement(2));
    if (!approx(rr, refRPY[0], EULER_EPS) ||
        !approx(rp, refRPY[1], EULER_EPS) ||
        !approx(ry, refRPY[2], EULER_EPS))
        return {"rpy_list_getter", false, "values mismatch"};

    return {"rpy_list_getter", true, "ok"};
}

static Result test_quat_getter() {
    VM& vm = VM::instance();
    struct Case { double r,p,y; const char* name; };
    Case cases[] = {
        {0, 0, 90*DEG, "yaw90"},
        {30*DEG, 45*DEG, 60*DEG, "r30p45y60"},
        {180*DEG, 0, 0, "roll180"},
        {-45*DEG, 20*DEG, -100*DEG, "negative"},
    };
    for (auto& c : cases) {
        auto ref = TestRefOrient::fromRPY(c.r, c.p, c.y);
        Value ov = Value::orientVal(eigenFromRPY(c.r, c.p, c.y));
        auto roxQuat = vec4FromValue(vm.orient_quat_getter(ov));
        if (!quatArrEqual(roxQuat, ref.getQuat()))
            return {"quat_getter", false, std::string("failed at ") + c.name};
    }
    return {"quat_getter", true, "4 cases ok"};
}

static Result test_mat_getter() {
    VM& vm = VM::instance();
    struct Case { double r,p,y; const char* name; };
    Case cases[] = {
        {0, 0, 90*DEG, "yaw90"},
        {30*DEG, 45*DEG, 60*DEG, "r30p45y60"},
        {0, 0, 0, "identity"},
        {180*DEG, 0, 0, "roll180"},
    };
    for (auto& c : cases) {
        auto ref = TestRefOrient::fromRPY(c.r, c.p, c.y);
        Value ov = Value::orientVal(eigenFromRPY(c.r, c.p, c.y));
        auto roxMat = mat3FromValue(vm.orient_mat_getter(ov));
        if (!arr9Equal(roxMat, ref.getMatrix()))
            return {"mat_getter", false, std::string("failed at ") + c.name};
    }
    return {"mat_getter", true, "4 cases ok"};
}

static Result test_axis_angle_getter() {
    VM& vm = VM::instance();
    double r = 30*DEG, p = 45*DEG, y = 60*DEG;
    auto ref = TestRefOrient::fromRPY(r, p, y);
    TestRefOrient::Vec3 refAxis; double refAngle;
    ref.getAxisAngle(refAxis, refAngle);

    Value ov = Value::orientVal(eigenFromRPY(r, p, y));
    auto roxAxis = vec3FromValue(vm.orient_axis_getter(ov));
    double roxAngle = quantityToRadians(vm.orient_angle_getter(ov));

    if (!approx(roxAngle, refAngle, 1e-8))
        return {"axis_angle_getter", false, "angle mismatch"};
    if (!arr3Equal(roxAxis, refAxis, 1e-8))
        return {"axis_angle_getter", false, "axis mismatch"};

    return {"axis_angle_getter", true, "ok"};
}

static Result test_inverse_getter() {
    VM& vm = VM::instance();
    double r = 30*DEG, p = 45*DEG, y = 60*DEG;
    auto ref = TestRefOrient::fromRPY(r, p, y);
    auto refInvQuat = ref.inverse().getQuat();

    Value ov = Value::orientVal(eigenFromRPY(r, p, y));
    Value inv = vm.orient_inverse_getter(ov);
    auto roxInvQuat = vec4FromValue(vm.orient_quat_getter(inv));

    if (!quatArrEqual(roxInvQuat, refInvQuat))
        return {"inverse_getter", false, "quat mismatch"};

    return {"inverse_getter", true, "ok"};
}

static Result test_rotate_method() {
    VM& vm = VM::instance();
    double r = 30*DEG, p = 45*DEG, y = 60*DEG;
    auto ref = TestRefOrient::fromRPY(r, p, y);
    auto refRot = ref.rotate({1.0, 2.0, 3.0});

    Value ov = Value::orientVal(eigenFromRPY(r, p, y));
    Eigen::VectorXd vecIn(3); vecIn << 1.0, 2.0, 3.0;
    Value vIn = Value::vectorVal(vecIn);
    Value args[] = {ov, vIn};
    Value roxRot = vm.orient_rotate_builtin(ArgsView(args, 2));
    auto roxVec = vec3FromValue(roxRot);

    if (!arr3Equal(roxVec, refRot, 1e-10))
        return {"rotate_method", false, "mismatch"};

    return {"rotate_method", true, "ok"};
}

static Result test_slerp_method() {
    VM& vm = VM::instance();
    auto ref0 = TestRefOrient();
    auto ref1 = TestRefOrient::fromRPY(0, 0, 90*DEG);
    auto refMid = TestRefOrient::slerp(ref0, ref1, 0.5);

    Value ov0 = Value::orientVal();
    Value ov1 = Value::orientVal(eigenFromRPY(0, 0, 90*DEG));
    Value tVal = Value::realVal(0.5);
    Value args[] = {ov0, ov1, tVal};
    Value roxMid = vm.orient_slerp_builtin(ArgsView(args, 3));
    auto roxQuat = vec4FromValue(vm.orient_quat_getter(roxMid));

    if (!quatArrEqual(roxQuat, refMid.getQuat()))
        return {"slerp_method", false, "t=0.5 mismatch"};

    return {"slerp_method", true, "ok"};
}

static Result test_angle_to_method() {
    VM& vm = VM::instance();
    auto ref0 = TestRefOrient();
    auto ref1 = TestRefOrient::fromRPY(0, 0, 90*DEG);
    double refAngle = TestRefOrient::angleBetween(ref0, ref1);

    Value ov0 = Value::orientVal();
    Value ov1 = Value::orientVal(eigenFromRPY(0, 0, 90*DEG));
    Value args[] = {ov0, ov1};
    Value roxAngle = vm.orient_angle_to_builtin(ArgsView(args, 2));
    double roxRad = quantityToRadians(roxAngle);

    if (!approx(roxRad, refAngle, 1e-10))
        return {"angle_to_method", false, "mismatch"};
    if (!approx(roxRad, 90*DEG, 1e-10))
        return {"angle_to_method", false, "not 90deg"};

    return {"angle_to_method", true, "ok"};
}

static Result test_euler_method() {
    VM& vm = VM::instance();
    double r = 30*DEG, p = 45*DEG, y = 60*DEG;
    auto ref = TestRefOrient::fromRPY(r, p, y);

    Value ov = Value::orientVal(eigenFromRPY(r, p, y));

    // Test ZXZ extraction
    // Roxal's .euler("ZXZ") uses Eigen canonicalEulerAngles(Z,X,Z) = intrinsic
    // TestRefOrient uses extrinsic (static) ZXZs.
    // Both should produce angles that reconstruct to the same rotation, even if
    // the angles themselves differ (intrinsic vs extrinsic decomposition).
    // So we verify round-trip: extract angles, reconstruct, compare quaternion.

    Value axesStr = Value::stringVal(toUnicodeString("ZXZ"));
    Value args[] = {ov, axesStr};
    Value roxEuler = vm.orient_euler_builtin(ArgsView(args, 2));

    if (!isList(roxEuler) || asList(roxEuler)->length() != 3)
        return {"euler_method", false, "not a 3-element list"};

    double ea0 = quantityToRadians(asList(roxEuler)->getElement(0));
    double ea1 = quantityToRadians(asList(roxEuler)->getElement(1));
    double ea2 = quantityToRadians(asList(roxEuler)->getElement(2));

    // Reconstruct using Eigen intrinsic (same as Roxal's orient(euler=, axes=))
    Eigen::Quaterniond recon =
        Eigen::AngleAxisd(ea0, Eigen::Vector3d::UnitZ())
      * Eigen::AngleAxisd(ea1, Eigen::Vector3d::UnitX())
      * Eigen::AngleAxisd(ea2, Eigen::Vector3d::UnitZ());

    // Compare against original
    auto origQuat = ref.getQuat();
    std::array<double,4> reconArr = {recon.x(), recon.y(), recon.z(), recon.w()};
    if (!quatArrEqual(reconArr, origQuat, 1e-8))
        return {"euler_method", false, "ZXZ round-trip mismatch"};

    return {"euler_method", true, "ok"};
}

static Result test_systematic() {
    VM& vm = VM::instance();
    double rolls[]  = {0, 45*DEG, 90*DEG, 135*DEG, -30*DEG, -90*DEG};
    double pitches[]= {0, 30*DEG, 60*DEG, 80*DEG, -20*DEG, -45*DEG};
    double yaws[]   = {0, 60*DEG, 120*DEG, 180*DEG, -60*DEG, -150*DEG};

    int count = 0;
    for (double r : rolls)
      for (double p : pitches)
        for (double y : yaws) {
            auto ref = TestRefOrient::fromRPY(r, p, y);
            Value ov = Value::orientVal(eigenFromRPY(r, p, y));

            // Check .quat getter
            auto roxQuat = vec4FromValue(vm.orient_quat_getter(ov));
            if (!quatArrEqual(roxQuat, ref.getQuat())) {
                std::ostringstream oss;
                oss << "quat r=" << r/DEG << " p=" << p/DEG << " y=" << y/DEG;
                return {"systematic", false, oss.str()};
            }

            // Check .mat getter
            auto roxMat = mat3FromValue(vm.orient_mat_getter(ov));
            if (!arr9Equal(roxMat, ref.getMatrix(), 1e-9)) {
                std::ostringstream oss;
                oss << "mat r=" << r/DEG << " p=" << p/DEG << " y=" << y/DEG;
                return {"systematic", false, oss.str()};
            }

            // Check .rotate method
            Eigen::VectorXd vecIn(3); vecIn << 1.0, 2.0, 3.0;
            Value vIn = Value::vectorVal(vecIn);
            Value args[] = {ov, vIn};
            auto roxRot = vec3FromValue(vm.orient_rotate_builtin(ArgsView(args, 2)));
            auto refRot = ref.rotate({1.0, 2.0, 3.0});
            if (!arr3Equal(roxRot, refRot, 1e-9)) {
                std::ostringstream oss;
                oss << "rotate r=" << r/DEG << " p=" << p/DEG << " y=" << y/DEG;
                return {"systematic", false, oss.str()};
            }

            count++;
        }
    return {"systematic", true, std::to_string(count) + " combos ok"};
}

// ---- Public entry point ----

Results testOrientConversions()
{
    Results results;
    auto run = [&](Result(*fn)()) {
        try {
            results.push_back(fn());
        } catch (std::exception& e) {
            results.push_back({"(exception)", false, std::string("exception: ") + e.what()});
        }
    };

    run(test_identity);
    run(test_rpy_getters);
    run(test_rpy_list_getter);
    run(test_quat_getter);
    run(test_mat_getter);
    run(test_axis_angle_getter);
    run(test_inverse_getter);
    run(test_rotate_method);
    run(test_slerp_method);
    run(test_angle_to_method);
    run(test_euler_method);
    run(test_systematic);

    return results;
}

} // namespace roxal
