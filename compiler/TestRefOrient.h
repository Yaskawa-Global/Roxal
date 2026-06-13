/* TestRefOrient — reference orientation class for cross-validating Roxal's
   Eigen-based orient conversions.

   Adapted from Orient (C)2002 David Jung <opensim@pobox.com>,
   Stripped of C++ interface conveniences
   (operator overloading, serialization, MatrixRef indirection) to keep it
   self-contained: only <cmath>, <array>, <stdexcept>, <string>, <algorithm>.

   Internal storage is always one of:
     - Quaternion  [x, y, z, w]   (4 doubles)
     - Matrix      row-major 3x3  (9 doubles)
     - Euler/RPY   [a0, a1, a2]   (3 doubles, radians)
*/

#ifndef TESTS_TESTREFORIENT_H
#define TESTS_TESTREFORIENT_H

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <algorithm>

// Free constexpr helper so it can be used in in-class static constexpr inits
constexpr unsigned char tro_eulerRep(unsigned char a, unsigned char p,
                                     unsigned char r, unsigned char f) {
    return static_cast<unsigned char>((((((a << 1) + p) << 1) + r) << 1) + f);
}

class TestRefOrient {
public:
    using byte = unsigned char;
    using Rep  = byte;

    // Axis indices (not an enum to avoid incomplete-type issues in constexpr)
    static constexpr byte X = 0, Y = 1, Z = 2, W = 3;

    // Frame / Repetition / Parity as plain constants
    static constexpr byte Static_ = 0, Rotating_ = 1;
    static constexpr byte NonRepeating_ = 0, Repeating_ = 1;
    static constexpr byte Even_ = 0, Odd_ = 1;

    // All 24 Euler conventions (static axes)
    static constexpr Rep EulerXYZs = tro_eulerRep(X, Even_, NonRepeating_, Static_);
    static constexpr Rep EulerXYXs = tro_eulerRep(X, Even_, Repeating_,    Static_);
    static constexpr Rep EulerXZYs = tro_eulerRep(X, Odd_,  NonRepeating_, Static_);
    static constexpr Rep EulerXZXs = tro_eulerRep(X, Odd_,  Repeating_,    Static_);
    static constexpr Rep EulerYZXs = tro_eulerRep(Y, Even_, NonRepeating_, Static_);
    static constexpr Rep EulerYZYs = tro_eulerRep(Y, Even_, Repeating_,    Static_);
    static constexpr Rep EulerYXZs = tro_eulerRep(Y, Odd_,  NonRepeating_, Static_);
    static constexpr Rep EulerYXYs = tro_eulerRep(Y, Odd_,  Repeating_,    Static_);
    static constexpr Rep EulerZXYs = tro_eulerRep(Z, Even_, NonRepeating_, Static_);
    static constexpr Rep EulerZXZs = tro_eulerRep(Z, Even_, Repeating_,    Static_);
    static constexpr Rep EulerZYXs = tro_eulerRep(Z, Odd_,  NonRepeating_, Static_);
    static constexpr Rep EulerZYZs = tro_eulerRep(Z, Odd_,  Repeating_,    Static_);

    // Rotating axes
    static constexpr Rep EulerZYXr = tro_eulerRep(X, Even_, NonRepeating_, Rotating_);
    static constexpr Rep EulerXYXr = tro_eulerRep(X, Even_, Repeating_,    Rotating_);
    static constexpr Rep EulerYZXr = tro_eulerRep(X, Odd_,  NonRepeating_, Rotating_);
    static constexpr Rep EulerXZXr = tro_eulerRep(X, Odd_,  Repeating_,    Rotating_);
    static constexpr Rep EulerXZYr = tro_eulerRep(Y, Even_, NonRepeating_, Rotating_);
    static constexpr Rep EulerYZYr = tro_eulerRep(Y, Even_, Repeating_,    Rotating_);
    static constexpr Rep EulerZXYr = tro_eulerRep(Y, Odd_,  NonRepeating_, Rotating_);
    static constexpr Rep EulerYXYr = tro_eulerRep(Y, Odd_,  Repeating_,    Rotating_);
    static constexpr Rep EulerYXZr = tro_eulerRep(Z, Even_, NonRepeating_, Rotating_);
    static constexpr Rep EulerZXZr = tro_eulerRep(Z, Even_, Repeating_,    Rotating_);
    static constexpr Rep EulerXYZr = tro_eulerRep(Z, Odd_,  NonRepeating_, Rotating_);
    static constexpr Rep EulerZYZr = tro_eulerRep(Z, Odd_,  Repeating_,    Rotating_);

    static constexpr Rep LastEuler = 23;
    static constexpr Rep EulerRPY  = EulerXYZs;
    static constexpr Rep Quat      = 24;
    static constexpr Rep Mat       = 25;
    static constexpr Rep RepEnd    = 26;

    static bool isEuler(Rep r) { return r <= LastEuler; }

    // ---- Types for results ----
    using Quat4  = std::array<double,4>; // [x, y, z, w]
    using Mat3   = std::array<double,9>; // row-major [m00 m01 m02 m10 ... m22]
    using Vec3   = std::array<double,3>;

    // ---- Construction ----

    TestRefOrient()
        : rep_(Quat), q_({0,0,0,1}), m_(), v_() {}

    static TestRefOrient fromQuat(double x, double y, double z, double w) {
        TestRefOrient o;
        o.rep_ = Quat;
        double n = std::sqrt(x*x + y*y + z*z + w*w);
        if (n < 1e-15) throw std::runtime_error("zero quaternion");
        o.q_ = {x/n, y/n, z/n, w/n};
        return o;
    }

    static TestRefOrient fromRPY(double roll, double pitch, double yaw) {
        TestRefOrient o;
        o.rep_ = EulerRPY;
        o.v_ = {roll, pitch, yaw};
        return o;
    }

    static TestRefOrient fromEuler(double a0, double a1, double a2, Rep convention) {
        if (!isEuler(convention))
            throw std::runtime_error("not an Euler convention");
        TestRefOrient o;
        o.rep_ = convention;
        o.v_ = {a0, a1, a2};
        return o;
    }

    static TestRefOrient fromMatrix(const Mat3& m) {
        TestRefOrient o;
        o.rep_ = Mat;
        o.m_ = m;
        return o;
    }

    static TestRefOrient fromAxisAngle(double ax, double ay, double az, double angle) {
        double n = std::sqrt(ax*ax + ay*ay + az*az);
        if (n < 1e-15) return TestRefOrient();
        ax /= n; ay /= n; az /= n;
        double ha = angle * 0.5;
        double s = std::sin(ha);
        return fromQuat(ax*s, ay*s, az*s, std::cos(ha));
    }

    // ---- Representation access ----
    Rep representation() const { return rep_; }

    Quat4 getQuat() const {
        TestRefOrient tmp(*this);
        tmp.changeRep(Quat);
        return tmp.q_;
    }

    Mat3 getMatrix() const {
        TestRefOrient tmp(*this);
        tmp.changeRep(Mat);
        return tmp.m_;
    }

    Vec3 getEuler(Rep convention) const {
        if (!isEuler(convention))
            throw std::runtime_error("not an Euler convention");
        TestRefOrient tmp(*this);
        tmp.changeRep(convention);
        return tmp.v_;
    }

    Vec3 getRPY() const { return getEuler(EulerRPY); }

    // ---- Operations ----

    static Quat4 quatMul(const Quat4& a, const Quat4& b) {
        return {
            a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1],
            a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0],
            a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3],
            a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2]
        };
    }

    static TestRefOrient concatenate(const TestRefOrient& r1, const TestRefOrient& r2) {
        Quat4 q1 = r1.getQuat();
        Quat4 q2 = r2.getQuat();
        Quat4 qr = quatMul(q2, q1);
        return fromQuat(qr[0], qr[1], qr[2], qr[3]);
    }

    TestRefOrient inverse() const {
        Quat4 q = getQuat();
        return fromQuat(-q[0], -q[1], -q[2], q[3]);
    }

    Vec3 rotate(const Vec3& p) const {
        Quat4 q = getQuat();
        double ux = q[0], uy = q[1], uz = q[2], uw = q[3];
        double tx = uy*p[2] - uz*p[1];
        double ty = uz*p[0] - ux*p[2];
        double tz = ux*p[1] - uy*p[0];
        return {
            p[0] + 2.0*(uw*tx + uy*tz - uz*ty),
            p[1] + 2.0*(uw*ty + uz*tx - ux*tz),
            p[2] + 2.0*(uw*tz + ux*ty - uy*tx)
        };
    }

    static TestRefOrient slerp(const TestRefOrient& from, const TestRefOrient& to, double t) {
        Quat4 q0 = from.getQuat();
        Quat4 q1 = to.getQuat();
        double dot = q0[0]*q1[0] + q0[1]*q1[1] + q0[2]*q1[2] + q0[3]*q1[3];
        if (dot < 0) {
            q1 = {-q1[0], -q1[1], -q1[2], -q1[3]};
            dot = -dot;
        }
        if (dot > 0.9995) {
            Quat4 r;
            for (int i = 0; i < 4; i++) r[i] = q0[i] + t*(q1[i]-q0[i]);
            double n = std::sqrt(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]+r[3]*r[3]);
            for (int i = 0; i < 4; i++) r[i] /= n;
            return fromQuat(r[0],r[1],r[2],r[3]);
        }
        double theta = std::acos(dot);
        double s0 = std::sin((1-t)*theta) / std::sin(theta);
        double s1 = std::sin(t*theta) / std::sin(theta);
        return fromQuat(
            s0*q0[0]+s1*q1[0], s0*q0[1]+s1*q1[1],
            s0*q0[2]+s1*q1[2], s0*q0[3]+s1*q1[3]);
    }

    static double angleBetween(const TestRefOrient& a, const TestRefOrient& b) {
        Quat4 qa = a.getQuat();
        Quat4 qb = b.getQuat();
        double dot = qa[0]*qb[0] + qa[1]*qb[1] + qa[2]*qb[2] + qa[3]*qb[3];
        return 2.0 * std::acos(std::min(std::abs(dot), 1.0));
    }

    void getAxisAngle(Vec3& axis, double& angle) const {
        Quat4 q = getQuat();
        double sinHalf = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2]);
        if (sinHalf < 1e-12) {
            axis = {0, 0, 1};
            angle = 0;
        } else {
            axis = {q[0]/sinHalf, q[1]/sinHalf, q[2]/sinHalf};
            angle = 2.0 * std::atan2(sinHalf, q[3]);
        }
    }

    bool equals(const TestRefOrient& o, double eps = 1e-10) const {
        Quat4 qa = getQuat();
        Quat4 qb = o.getQuat();
        double dotPos = 0, dotNeg = 0;
        for (int i = 0; i < 4; i++) {
            dotPos += (qa[i]-qb[i])*(qa[i]-qb[i]);
            dotNeg += (qa[i]+qb[i])*(qa[i]+qb[i]);
        }
        return std::min(dotPos, dotNeg) < eps*eps*4;
    }

private:
    Rep rep_;
    Quat4 q_;
    Mat3  m_;
    Vec3  v_;

    // ---- Euler decode helpers (same bit-layout as yascore) ----
    static constexpr byte EulSafe[4] = {0,1,2,0};
    static constexpr byte EulNext[4] = {1,2,0,1};

    struct EulerParams {
        byte i,j,k,h;
        byte n; // parity
        byte s; // repetition
        byte f; // frame
    };

    static EulerParams decodeRep(Rep r) {
        EulerParams p;
        byte o = r;
        p.f = o & 1;        o >>= 1;
        p.s = o & 1;        o >>= 1;
        p.n = o & 1;        o >>= 1;
        p.i = EulSafe[o & 3];
        p.j = EulNext[p.i + p.n];
        p.k = EulNext[p.i + 1 - p.n];
        p.h = (p.s == NonRepeating_) ? p.k : p.i;
        return p;
    }

    double& M(int r, int c)       { return m_[r*3+c]; }
    double  M(int r, int c) const { return m_[r*3+c]; }

    // ---- Core conversion ----
    void changeRep(Rep newRep) {
        if (rep_ == newRep) return;

        if (isEuler(rep_)) {
            if (newRep == Quat) { eulerToQuat(); return; }
            if (newRep == Mat)  { eulerToMat();  return; }
            if (isEuler(newRep)) {
                eulerToQuat();
                quatToMat();
                rep_ = Mat;
                matToEuler(newRep);
                return;
            }
        }

        if (rep_ == Quat) {
            if (newRep == Mat) { quatToMat(); rep_ = Mat; return; }
            if (isEuler(newRep)) { quatToMat(); rep_ = Mat; matToEuler(newRep); return; }
        }

        if (rep_ == Mat) {
            if (newRep == Quat) { matToQuat(); rep_ = Quat; return; }
            if (isEuler(newRep)) { matToEuler(newRep); return; }
        }

        throw std::runtime_error("unsupported conversion");
    }

    void eulerToQuat() {
        auto p = decodeRep(rep_);
        double lv[3] = {v_[0], v_[1], v_[2]};
        if (p.f == Rotating_)  std::swap(lv[X], lv[Z]);
        if (p.n == Odd_)       lv[Y] = -lv[Y];

        double ti = lv[X]*0.5, tj = lv[Y]*0.5, th = lv[Z]*0.5;
        double ci = std::cos(ti), cj = std::cos(tj), ch = std::cos(th);
        double si = std::sin(ti), sj = std::sin(tj), sh = std::sin(th);
        double cc = ci*ch, cs = ci*sh, sc = si*ch, ss = si*sh;

        double a[3]; double w;
        if (p.s == Repeating_) {
            a[p.i] = cj*(cs + sc);
            a[p.j] = sj*(cc + ss);
            a[p.k] = sj*(cs - sc);
            w       = cj*(cc - ss);
        } else {
            a[p.i] = cj*sc - sj*cs;
            a[p.j] = cj*ss + sj*cc;
            a[p.k] = cj*cs - sj*sc;
            w       = cj*cc + sj*ss;
        }
        if (p.n == Odd_) a[p.j] = -a[p.j];

        q_ = {a[X], a[Y], a[Z], w};
        rep_ = Quat;
    }

    void eulerToMat() {
        auto p = decodeRep(rep_);
        double lv[3] = {v_[0], v_[1], v_[2]};
        if (p.f == Rotating_) std::swap(lv[X], lv[Z]);
        if (p.n == Odd_) { lv[X] = -lv[X]; lv[Y] = -lv[Y]; lv[Z] = -lv[Z]; }

        double ti = lv[X], tj = lv[Y], th = lv[Z];
        double ci = std::cos(ti), cj = std::cos(tj), ch = std::cos(th);
        double si = std::sin(ti), sj = std::sin(tj), sh = std::sin(th);
        double cc = ci*ch, cs = ci*sh, sc = si*ch, ss = si*sh;

        m_.fill(0);
        if (p.s == Repeating_) {
            M(p.i,p.i) = cj;       M(p.i,p.j) =  sj*si;      M(p.i,p.k) =  sj*ci;
            M(p.j,p.i) = sj*sh;    M(p.j,p.j) = -cj*ss+cc;   M(p.j,p.k) = -cj*cs-sc;
            M(p.k,p.i) = -sj*ch;   M(p.k,p.j) =  cj*sc+cs;   M(p.k,p.k) =  cj*cc-ss;
        } else {
            M(p.i,p.i) = cj*ch;    M(p.i,p.j) = sj*sc-cs;    M(p.i,p.k) = sj*cc+ss;
            M(p.j,p.i) = cj*sh;    M(p.j,p.j) = sj*ss+cc;    M(p.j,p.k) = sj*cs-sc;
            M(p.k,p.i) = -sj;      M(p.k,p.j) = cj*si;       M(p.k,p.k) = cj*ci;
        }
        rep_ = Mat;
    }

    void quatToMat() {
        double nq = q_[0]*q_[0]+q_[1]*q_[1]+q_[2]*q_[2]+q_[3]*q_[3];
        double s = (nq > 0) ? (2.0/std::sqrt(nq)) : 0.0;

        double xs = q_[0]*s, ys = q_[1]*s, zs = q_[2]*s;
        double wx = q_[3]*xs, wy = q_[3]*ys, wz = q_[3]*zs;
        double xx = q_[0]*xs, xy = q_[0]*ys, xz = q_[0]*zs;
        double yy = q_[1]*ys, yz = q_[1]*zs, zz = q_[2]*zs;

        m_[0] = 1-(yy+zz); m_[1] = xy-wz;     m_[2] = xz+wy;
        m_[3] = xy+wz;      m_[4] = 1-(xx+zz); m_[5] = yz-wx;
        m_[6] = xz-wy;      m_[7] = yz+wx;      m_[8] = 1-(xx+yy);
    }

    void matToQuat() {
        static constexpr double eps = 1e-12;
        double tr = 1.0 + M(0,0) + M(1,1) + M(2,2);
        if (tr > eps) {
            double s = std::sqrt(tr)*2.0;
            q_[0] = (M(2,1)-M(1,2))/s;
            q_[1] = (M(0,2)-M(2,0))/s;
            q_[2] = (M(1,0)-M(0,1))/s;
            q_[3] = 0.25*s;
        } else if (M(0,0) > M(1,1) && M(0,0) > M(2,2)) {
            double s = std::sqrt(1+M(0,0)-M(1,1)-M(2,2))*2.0;
            q_[0] = 0.25*s;
            q_[1] = (M(1,0)+M(0,1))/s;
            q_[2] = (M(0,2)+M(2,0))/s;
            q_[3] = (M(2,1)-M(1,2))/s;
        } else if (M(1,1) > M(2,2)) {
            double s = std::sqrt(1+M(1,1)-M(0,0)-M(2,2))*2.0;
            q_[0] = (M(1,0)+M(0,1))/s;
            q_[1] = 0.25*s;
            q_[2] = (M(2,1)+M(1,2))/s;
            q_[3] = (M(0,2)-M(2,0))/s;
        } else {
            double s = std::sqrt(1+M(2,2)-M(0,0)-M(1,1))*2.0;
            q_[0] = (M(0,2)+M(2,0))/s;
            q_[1] = (M(2,1)+M(1,2))/s;
            q_[2] = 0.25*s;
            q_[3] = (M(1,0)-M(0,1))/s;
        }
        rep_ = Quat;
    }

    void matToEuler(Rep newRep) {
        auto p = decodeRep(newRep);
        double lv[3];

        if (p.s == Repeating_) {
            double sy = std::sqrt(M(p.i,p.j)*M(p.i,p.j) + M(p.i,p.k)*M(p.i,p.k));
            if (sy > 16*std::numeric_limits<double>::min()) {
                lv[X] = std::atan2(M(p.i,p.j), M(p.i,p.k));
                lv[Y] = std::atan2(sy, M(p.i,p.i));
                lv[Z] = std::atan2(M(p.j,p.i), -M(p.k,p.i));
            } else {
                lv[X] = std::atan2(-M(p.j,p.k), M(p.j,p.j));
                lv[Y] = std::atan2(sy, M(p.i,p.i));
                lv[Z] = 0;
            }
        } else {
            double cy = std::sqrt(M(p.i,p.i)*M(p.i,p.i) + M(p.j,p.i)*M(p.j,p.i));
            if (cy > 16*std::numeric_limits<double>::min()) {
                lv[X] = std::atan2(M(p.k,p.j), M(p.k,p.k));
                lv[Y] = std::atan2(-M(p.k,p.i), cy);
                lv[Z] = std::atan2(M(p.j,p.i), M(p.i,p.i));
            } else {
                lv[X] = std::atan2(-M(p.j,p.k), M(p.j,p.j));
                lv[Y] = std::atan2(-M(p.k,p.i), cy);
                lv[Z] = 0;
            }
        }
        if (p.n == Odd_) { lv[X] = -lv[X]; lv[Y] = -lv[Y]; lv[Z] = -lv[Z]; }
        if (p.f == Rotating_) std::swap(lv[X], lv[Z]);

        v_ = {lv[0], lv[1], lv[2]};
        rep_ = newRep;
    }
};

#endif // TESTS_TESTREFORIENT_H
