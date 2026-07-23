#!/usr/bin/env python3
"""Generate the mechanical layer of the Roxal opencv module from OpenCV's own
Python-binding metadata (hdr_parser.py in the OpenCV source tree).

Emits:
  - enum constant groups (values read from the parsed headers, never transcribed
    by hand) and the _cvt_channels helper, spliced into ../init.rox between the
    BEGIN/END GENERATED markers
  - cvx_gen.inc: C shim functions for the uniform same-shape filter family,
    #included by cvx_shim.cpp

Run after bumping the vendored OpenCV or editing the spec tables below:
    python3 generate.py && ./build.sh
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OPENCV_SRC = os.path.join(HERE, '..', '..', '..', 'deps', 'opencv')
INIT_ROX = os.path.join(HERE, '..', 'init.rox')
GEN_INC = os.path.join(HERE, 'cvx_gen.inc')

BEGIN_MARK = '# ==== BEGIN GENERATED (shim/generate.py) — do not edit by hand ===='
END_MARK = '# ==== END GENERATED ===='

HEADERS = [
    'modules/core/include/opencv2/core.hpp',
    'modules/imgproc/include/opencv2/imgproc.hpp',
    'modules/videoio/include/opencv2/videoio.hpp',
    'modules/objdetect/include/opencv2/objdetect/aruco_dictionary.hpp',
    'modules/calib/include/opencv2/calib.hpp',
]

# DICT_4X4_50 -> Dict4x4_50 etc. (enum names may not start with a digit)
ARUCO_RENAME = {f'{n}X{n}_{c}': f'Dict{n}x{n}_{c}'
                for n in (4, 5, 6, 7) for c in (50, 100, 250, 1000)}
ARUCO_RENAME.update({'ARUCO_ORIGINAL': 'Original',
                     'APRILTAG_16h5': 'AprilTag16h5', 'APRILTAG_25h9': 'AprilTag25h9',
                     'APRILTAG_36h10': 'AprilTag36h10', 'APRILTAG_36h11': 'AprilTag36h11'})

# Color-conversion sides exposed at the Roxal API (images are RGB there; plain
# BGR codes are meaningless to a Roxal user and are filtered out).
COLOR_SIDES = ['RGB', 'RGBA', 'GRAY', 'HSV', 'HLS', 'Lab', 'Luv', 'YUV', 'YCrCb', 'XYZ']


def camel(name):
    return ''.join(p.capitalize() for p in name.split('_') if p)


def color_entry(name):
    """COLOR_RGB2GRAY -> ('Rgb2Gray', 1-channel-target) or None if filtered."""
    m = re.fullmatch('(' + '|'.join(COLOR_SIDES) + ')2(' + '|'.join(COLOR_SIDES) + ')', name)
    if not m:
        return None
    src, dst = m.group(1), m.group(2)
    chans = 1 if dst == 'GRAY' else 4 if dst == 'RGBA' else 3
    return (src.capitalize() + '2' + dst.capitalize(), chans)


ENUM_SPECS = [
    dict(cpp='InterpolationFlags', rox='Interpolation', strip='INTER_',
         include=['NEAREST', 'LINEAR', 'CUBIC', 'AREA', 'LANCZOS4',
                  'LINEAR_EXACT', 'NEAREST_EXACT']),
    dict(cpp='ThresholdTypes', rox='ThresholdType', strip='THRESH_',
         include=['BINARY', 'BINARY_INV', 'TRUNC', 'TOZERO', 'TOZERO_INV',
                  'OTSU', 'TRIANGLE']),
    dict(cpp='ColorConversionCodes', rox='ColorCode', strip='COLOR_', color=True),
    dict(cpp='MorphShapes', rox='MorphShape', strip='MORPH_',
         include=['RECT', 'CROSS', 'ELLIPSE']),
    dict(cpp='MorphTypes', rox='MorphOp', strip='MORPH_',
         include=['OPEN', 'CLOSE', 'GRADIENT', 'TOPHAT', 'BLACKHAT']),
    dict(cpp='RetrievalModes', rox='ContourMode', strip='RETR_',
         include=['EXTERNAL', 'LIST', 'CCOMP', 'TREE']),
    dict(cpp='ContourApproximationModes', rox='ContourApprox', strip='CHAIN_APPROX_',
         include=['NONE', 'SIMPLE', 'TC89_L1', 'TC89_KCOS']),
    dict(cpp='HersheyFonts', rox='Font', strip='FONT_HERSHEY_',
         include=['SIMPLEX', 'PLAIN', 'DUPLEX', 'COMPLEX', 'TRIPLEX',
                  'COMPLEX_SMALL', 'SCRIPT_SIMPLEX', 'SCRIPT_COMPLEX']),
    dict(cpp='RotateFlags', rox='Rotation', strip='ROTATE_',
         rename={'90_CLOCKWISE': 'Clockwise90', '180': 'Half',
                 '90_COUNTERCLOCKWISE': 'CounterClockwise90'}),
    dict(cpp='VideoCaptureProperties', rox='CapProp', strip='CAP_PROP_',
         include=['POS_MSEC', 'POS_FRAMES', 'FRAME_WIDTH', 'FRAME_HEIGHT',
                  'FPS', 'FOURCC', 'FRAME_COUNT', 'BRIGHTNESS', 'CONTRAST',
                  'SATURATION', 'EXPOSURE', 'AUTO_EXPOSURE', 'GAIN', 'BUFFERSIZE']),
    dict(cpp='PredefinedDictionaryType', rox='ArucoDict', strip='DICT_',
         rename=ARUCO_RENAME),
    dict(cpp='HandEyeCalibrationMethod', rox='HandEyeMethod', strip='CALIB_HAND_EYE_',
         include=['TSAI', 'PARK', 'HORAUD', 'ANDREFF', 'DANIILIDIS']),
    dict(cpp='TemplateMatchModes', rox='TemplateMethod', strip='TM_',
         include=['SQDIFF', 'SQDIFF_NORMED', 'CCORR', 'CCORR_NORMED',
                  'CCOEFF', 'CCOEFF_NORMED']),
]

# Uniform same-shape filter family: one spec row -> C shim + @cfunc decl + wrapper.
# params: (ctype, name, rox_type, rox_default or None)
FILTERS = [
    dict(name='median_blur', chan='any',
         params=[('int', 'ksize', 'int', None)],
         call='cv::medianBlur(s, d, ksize);',
         doc='Median filter; ksize must be odd.'),
    dict(name='blur', chan='any',
         params=[('int', 'ksize', 'int', None)],
         call='cv::blur(s, d, cv::Size(ksize, ksize));',
         doc='Box blur.'),
    dict(name='bilateral_filter', chan='any',
         params=[('int', 'diameter', 'int', None),
                 ('double', 'sigma_color', 'real', None),
                 ('double', 'sigma_space', 'real', None)],
         call='cv::bilateralFilter(s, d, diameter, sigma_color, sigma_space);',
         doc='Edge-preserving smoothing.'),
    dict(name='equalize_hist', chan='gray', params=[],
         call='cv::equalizeHist(s, d);',
         doc='Histogram equalization (grayscale).'),
    dict(name='sobel', chan='gray',
         params=[('int', 'dx', 'int', None), ('int', 'dy', 'int', None),
                 ('int', 'ksize', 'int', '3')],
         call='{ cv::Mat t16; cv::Sobel(s, t16, CV_16S, dx, dy, ksize); cv::convertScaleAbs(t16, d); }',
         doc='Sobel derivative magnitude (grayscale).'),
    dict(name='laplacian', chan='gray',
         params=[('int', 'ksize', 'int', '3')],
         call='{ cv::Mat t16; cv::Laplacian(s, t16, CV_16S, ksize); cv::convertScaleAbs(t16, d); }',
         doc='Laplacian edge response (grayscale).'),
    dict(name='erode', chan='any',
         params=[('int', 'ksize', 'int', None), ('int', 'iterations', 'int', '1'),
                 ('int', 'kshape', '', 'MorphShape.Rect')],
         call='cv::erode(s, d, cv::getStructuringElement(kshape, cv::Size(ksize, ksize)), cv::Point(-1, -1), iterations);',
         doc='Morphological erosion.'),
    dict(name='dilate', chan='any',
         params=[('int', 'ksize', 'int', None), ('int', 'iterations', 'int', '1'),
                 ('int', 'kshape', '', 'MorphShape.Rect')],
         call='cv::dilate(s, d, cv::getStructuringElement(kshape, cv::Size(ksize, ksize)), cv::Point(-1, -1), iterations);',
         doc='Morphological dilation.'),
    dict(name='morphology', chan='any',
         params=[('int', 'op', '', None), ('int', 'ksize', 'int', None),
                 ('int', 'iterations', 'int', '1'), ('int', 'kshape', '', 'MorphShape.Rect')],
         call='cv::morphologyEx(s, d, op, cv::getStructuringElement(kshape, cv::Size(ksize, ksize)), cv::Point(-1, -1), iterations);',
         doc='Morphological operation (see MorphOp).'),
    dict(name='flip', chan='any',
         params=[('int', 'axis', 'int', None)],
         call='cv::flip(s, d, axis);',
         doc='Flip: axis 0 = vertical, 1 = horizontal, -1 = both.'),
]


def parse_constants():
    sys.path.insert(0, os.path.join(OPENCV_SRC, 'modules', 'python', 'src2'))
    import hdr_parser
    parser = hdr_parser.CppHeaderParser(
        generate_umat_decls=False, generate_gpumat_decls=False,
        preprocessor_definitions={'CV_VERSION_MAJOR': 5, 'CV_VERSION_MINOR': 0,
                                  'CV_VERSION_REVISION': 0})
    enums = {}   # cpp enum name (last component) -> [(const name, int value)]
    env = {}     # all constants seen, for resolving symbolic values
    for hdr in HEADERS:
        for d in parser.parse(os.path.join(OPENCV_SRC, hdr), wmode=False):
            if not d[0].startswith('enum'):
                continue
            ename = d[0].split()[-1].split('.')[-1]
            items = []
            prev = -1
            for c in d[3]:
                cname = c[0].split()[-1].split('.')[-1]
                sval = (c[1] or '').strip()
                if sval == '':
                    val = prev + 1
                else:
                    expr = re.sub(r'\bcv::|\bcv\.', '', sval)
                    expr = re.sub(r'[A-Za-z_][A-Za-z_0-9]*',
                                  lambda m: str(env[m.group(0)]) if m.group(0) in env else m.group(0),
                                  expr)
                    try:
                        val = int(eval(expr, {'__builtins__': {}}))
                    except Exception:
                        continue
                env[cname] = val
                prev = val
                items.append((cname, val))
            enums.setdefault(ename, []).extend(items)
    return enums


def gen_enums(enums):
    out = []
    color_channels = []
    for spec in ENUM_SPECS:
        items = enums.get(spec['cpp'])
        if not items:
            print(f"warning: enum {spec['cpp']} not found", file=sys.stderr)
            continue
        entries = []
        seen_names, seen_vals = set(), set()
        for cname, val in items:
            if not cname.startswith(spec['strip']):
                continue
            short = cname[len(spec['strip']):]
            if spec.get('color'):
                ce = color_entry(short)
                if not ce:
                    continue
                rox_name = ce[0]
                color_channels.append((rox_name, ce[1]))
            elif 'rename' in spec:
                if short not in spec['rename']:
                    continue
                rox_name = spec['rename'][short]
            else:
                if spec.get('include') is not None and short not in spec['include']:
                    continue
                rox_name = camel(short)
            if rox_name in seen_names or val in seen_vals:
                continue
            seen_names.add(rox_name)
            seen_vals.add(val)
            entries.append((rox_name, val))
        out.append(f'type {spec["rox"]} enum:')
        for rox_name, val in entries:
            out.append(f'  {rox_name} = {val}')
        out.append('')
    return '\n'.join(out), color_channels


def gen_cvt_channels(color_channels):
    # enum-member comparisons: Roxal enum == raw-int compares are (correctly) false
    lines = ['# destination channel count for each ColorCode (generated from the codes above)',
             'func _cvt_channels(code) -> int:']
    for name, chans in color_channels:
        if chans != 3:
            lines.append(f'  if code == ColorCode.{name}:')
            lines.append(f'    return {chans}')
    lines.append('  return 3')
    lines.append('')
    return '\n'.join(lines)


def gen_filters():
    c_parts = ['// Generated by generate.py — do not edit. Same-shape filter family.',
               'extern "C" {', '']
    rox_decls = []
    rox_wrappers = []
    for f in FILTERS:
        name = f['name']
        cparams = ''.join(f', {ct} {pn}' for ct, pn, _, _ in f['params'])
        c_parts.append(
            f'int cvx_gen_{name}(const void* src, int h, int w, int c, void* dst{cparams})\n'
            '{\n'
            '    CVX_TRY\n'
            '    cv::Mat s = borrow(src, h, w, c);\n'
            '    cv::Mat d = borrow(dst, h, w, c);\n'
            '    void* dp = d.data;\n'
            f'    {f["call"]}\n'
            f'    if (d.data != dp) {{ g_lastError = "{name}: output buffer mismatch"; return -1; }}\n'
            '    return 0;\n'
            '    CVX_CATCH(-1)\n'
            '}\n')

        arg_str = 'const void* src, int h, int w, int c, void* dst' + cparams
        decl_params = ''.join(
            f', {pn}: {rt}' if rt else f', {pn}' for _, pn, rt, _ in f['params'])
        rox_decls.append(
            f"@cfunc(lib=cvxlib, cname='cvx_gen_{name}', args='{arg_str}', ret='int')\n"
            f'func _gen_{name}(src, h: int, w: int, c: int, dst{decl_params}) -> int:\n'
            '  _\n')

        sig_params = ''
        pass_params = ''
        for _, pn, rt, dflt in f['params']:
            typed = f'{pn}: {rt}' if rt else pn
            sig_params += f', {typed} = {dflt}' if dflt is not None else f', {typed}'
            pass_params += f', {pn}'
        body = [f'# {f["doc"]}',
                f'func {name}(img: tensor{sig_params}) -> tensor:',
                '  var s = img.shape()']
        if f['chan'] == 'gray':
            body.append(f"  if s[2] != 1:")
            body.append(f"    raise RuntimeException('{name}: expects a grayscale image (use grayscale() first)')")
        body.append("  var out = tensor([s[0], s[1], s[2]], dtype='uint8')")
        body.append(f'  if _gen_{name}(img, s[0], s[1], s[2], out{pass_params}) != 0:')
        body.append(f"    raise RuntimeException('{name}: ' + _last_error())")
        body.append('  return out')
        body.append('')
        rox_wrappers.append('\n'.join(body))
    c_parts.append('} // extern "C"')
    return '\n'.join(c_parts), '\n'.join(rox_decls), '\n'.join(rox_wrappers)


def main():
    enums = parse_constants()
    enum_rox, color_channels = gen_enums(enums)
    cvt_rox = gen_cvt_channels(color_channels)
    c_code, decl_rox, wrapper_rox = gen_filters()

    with open(GEN_INC, 'w') as fh:
        fh.write(c_code + '\n')

    gen_section = '\n'.join([
        BEGIN_MARK,
        '',
        enum_rox,
        cvt_rox,
        decl_rox,
        wrapper_rox,
        END_MARK,
    ])
    with open(INIT_ROX) as fh:
        content = fh.read()
    begin = content.find(BEGIN_MARK)
    end = content.find(END_MARK)
    if begin < 0 or end < 0:
        print('error: GENERATED markers not found in init.rox', file=sys.stderr)
        sys.exit(1)
    content = content[:begin] + gen_section + content[end + len(END_MARK):]
    with open(INIT_ROX, 'w') as fh:
        fh.write(content)
    print(f'wrote {GEN_INC} and regenerated section in init.rox '
          f'({len(FILTERS)} filters, {enum_rox.count("type ")} enums)')


if __name__ == '__main__':
    main()
