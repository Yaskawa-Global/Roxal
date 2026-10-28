#!/usr/bin/env python3
"""Generate the static-FFI registration table for a Roxal FFI module.

A wasm build cannot dlopen, so a library that a module reaches through
sys.loadlib + @cfunc is linked into the host instead and registered with
roxal::registerStaticLibrary (compiler/FFIStatic.h).  This script reads the
module's .rox, and writes a C++ fragment for the library's OWN translation unit
(included at its end) holding:

  - an `extern "C"` prototype for every @cfunc cname and free= function, spelled
    from the declaration's args=/ret= strings.  Compiled in the same unit as the
    definitions, any drift between the .rox declaration and the C definition is
    a compile error ("conflicting declaration of C function") instead of a
    wasm-ld signature-mismatch stub that traps when called;
  - the symbol table;
  - `extern "C" void roxal_ffi_register_<stem>()`, which the host calls once at
    startup (for libcvxshim.so: roxal_ffi_register_cvxshim).

Usage:
    ffi_static_registry.py <module.rox> -o <table.inc> [--library libfoo.so]
"""

import argparse
import os
import re
import sys

ROX_TO_C = {'real': 'double'}


def split_args(text):
    """Split an annotation's argument list on top-level commas (quotes respected)."""
    parts, cur, quote = [], [], None
    for ch in text:
        if quote:
            cur.append(ch)
            if ch == quote:
                quote = None
        elif ch in ('"', "'"):
            quote = ch
            cur.append(ch)
        elif ch == ',':
            parts.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if ''.join(cur).strip():
        parts.append(''.join(cur).strip())
    return parts


def unquote(v):
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in ('"', "'"):
        return v[1:-1]
    return v


def c_type(t):
    t = ' '.join(t.split())
    const = t.startswith('const ')
    base = t[6:] if const else t
    stars = ''
    while base.endswith('*') or base.endswith('&'):
        stars = base[-1] + stars
        base = base[:-1].rstrip()
    base = ROX_TO_C.get(base, base)
    return ('const ' if const else '') + base + (' ' + stars if stars else '')


def c_params(args):
    """args='const void* src, int h' -> 'const void* src, int h' (types normalised)."""
    if not args.strip():
        return 'void'
    out = []
    for i, p in enumerate(split_args(args)):
        p = ' '.join(p.split())
        # The last word is the parameter name unless the token is a bare type.
        m = re.match(r'^(.*?[\s\*&])([A-Za-z_][A-Za-z_0-9]*)$', p)
        if m and m.group(1).strip():
            ty, name = m.group(1).strip(), m.group(2)
        else:
            ty, name = p, 'a%d' % i
        out.append('%s %s' % (c_type(ty), name))
    return ', '.join(out)


def parse(rox_path):
    src = open(rox_path, encoding='utf-8').read()
    libs = re.findall(r"sys\.loadlib\(\s*['\"]([^'\"]+)['\"]\s*\)", src)
    lines = src.splitlines()
    funcs = {}      # cname -> (ret, params, line)
    frees = {}      # name -> line
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r'\s*@cfunc\((.*)\)\s*$', line)
        if m:
            kv = {}
            for part in split_args(m.group(1)):
                if '=' not in part:
                    continue
                k, v = part.split('=', 1)
                kv[k.strip()] = unquote(v)
            cname = kv.get('cname')
            if not cname:
                # the function the annotation decorates (skipping further annotations)
                j = i + 1
                while j < len(lines) and lines[j].strip().startswith('@'):
                    j += 1
                fm = re.match(r'\s*(?:func|proc)\s+([A-Za-z_][A-Za-z_0-9]*)', lines[j] if j < len(lines) else '')
                if not fm:
                    sys.exit('%s:%d: @cfunc without cname= and no func/proc after it' % (rox_path, i + 1))
                cname = fm.group(1)
            ret = c_type(kv.get('ret', 'void'))
            params = c_params(kv.get('args', ''))
            prev = funcs.get(cname)
            if prev and (prev[0], prev[1]) != (ret, params):
                sys.exit('%s:%d: %s declared again with a different signature (first at line %d)'
                         % (rox_path, i + 1, cname, prev[2]))
            funcs[cname] = (ret, params, i + 1)
            if kv.get('free'):
                frees.setdefault(kv['free'], i + 1)
        i += 1
    return libs, funcs, frees


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rox')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--library', help='library file name (default: the sys.loadlib argument in the .rox)')
    a = ap.parse_args()

    libs, funcs, frees = parse(a.rox)
    library = a.library
    if not library:
        names = sorted({os.path.basename(l) for l in libs})
        if len(names) != 1:
            sys.exit('%s: expected exactly one sys.loadlib(...) (found %s); pass --library' % (a.rox, names or 'none'))
        library = names[0]
    stem = re.sub(r'\.(so|dylib|dll)(\..*)?$', '', os.path.basename(library))
    if stem.startswith('lib'):
        stem = stem[3:]
    stem = re.sub(r'[^A-Za-z0-9_]', '_', stem)
    if not funcs:
        sys.exit('%s: no @cfunc declarations' % a.rox)

    rel = os.path.basename(a.rox)
    out = []
    out.append('// GENERATED by tools/ffi_static_registry.py from %s -- do not edit.' % rel)
    out.append('// Static-FFI registration for %s (compiler/FFIStatic.h): include at the END' % library)
    out.append('// of the library\'s own translation unit, after every definition.')
    out.append('#include "FFIStatic.h"')
    out.append('')
    out.append('extern "C" {')
    for name in sorted(funcs):
        ret, params, line = funcs[name]
        out.append('%s %s(%s);   // %s:%d' % (ret, name, params, rel, line))
    for name in sorted(frees):
        if name not in funcs:
            out.append('void %s(void* p);   // free= (%s:%d)' % (name, rel, frees[name]))
    out.append('}')
    out.append('')
    symbols = sorted(set(funcs) | set(frees))
    out.append('static const roxal::StaticFFISymbol roxal_ffi_symbols_%s[] = {' % stem)
    for name in symbols:
        out.append('    {"%s", reinterpret_cast<void*>(&%s)},' % (name, name))
    out.append('};')
    out.append('')
    out.append('extern "C" void roxal_ffi_register_%s()' % stem)
    out.append('{')
    out.append('    roxal::registerStaticLibrary("%s", roxal_ffi_symbols_%s,' % (library, stem))
    out.append('                                 sizeof(roxal_ffi_symbols_%s) / sizeof(roxal_ffi_symbols_%s[0]));'
               % (stem, stem))
    out.append('}')
    text = '\n'.join(out) + '\n'

    # Only touch the file when it changes, so an unchanged .rox does not rebuild the library.
    try:
        if open(a.output, encoding='utf-8').read() == text:
            return
    except OSError:
        pass
    os.makedirs(os.path.dirname(os.path.abspath(a.output)), exist_ok=True)
    with open(a.output, 'w', encoding='utf-8') as f:
        f.write(text)


if __name__ == '__main__':
    main()
