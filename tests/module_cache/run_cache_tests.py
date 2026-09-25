"""Bytecode-cache (.roc) scenarios: several roxal runs per case, in a private
directory, checking what the cache does BETWEEN runs -- invalidation, import
order, once-per-VM bodies, sizes -- which a single .rox/.out pair cannot.

runtests.py lists each scenario as `modcache_<name>` and calls run(); the
scenarios pass their own --nocache/--recompile flags, so they are unaffected
by the harness's cache options.  Run standalone with:

    python3 tests/module_cache/run_cache_tests.py build/roxal [name ...]
"""
import os
import shutil
import subprocess
import sys
import tempfile

# Module names carry a prefix no test in tests/ uses, so a scenario cannot be
# shadowed by (or shadow) anything on a wider search path.
PFX = 'mc_'


class Scenario:
    """A scratch directory plus helpers; `fail` raises with a message."""

    class Failed(Exception):
        pass

    def __init__(self, roxal, env, timeout):
        self.roxal = roxal
        self.timeout = timeout
        self.dir = tempfile.mkdtemp(prefix='roxal_modcache_')
        self.env = dict(env)
        self.env['ROXALPATH'] = self.dir          # this directory, nothing else

    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def write(self, name, text):
        with open(os.path.join(self.dir, name), 'w', encoding='utf-8') as f:
            f.write(text)

    def module(self, name, imports=(), funcs=1, body_print=True, extra=''):
        """A module printing 'body <name>' once, with `funcs` small functions."""
        lines = [f'import {m}' for m in imports]
        if body_print:
            lines.append(f"print('body {name}', flush=true)")
        for i in range(1, funcs + 1):
            lines += [f'func {name}_f{i}(x):', '  var s = 0', '  var i = 0',
                      '  while i < x:', f'    s = s + i * {i}', '    i = i + 1',
                      f'  return s + {i}']
        lines += [f'func {name}_ver():', f"  return '{name}-v1'"]
        if extra:
            lines.append(extra)
        self.write(name + '.rox', '\n'.join(lines) + '\n')

    def run(self, script, *flags, expect_ok=True, env=None):
        """stdout of `roxal <flags> <script>`; fails on a non-zero exit.
        `env` overrides variables for this run (e.g. a different ROXALPATH)."""
        cmd = [self.roxal, *flags, script]
        run_env = dict(self.env, **(env or {}))
        proc = subprocess.run(cmd, cwd=self.dir, env=run_env, capture_output=True,
                              timeout=self.timeout)
        out = proc.stdout.decode(errors='replace')
        if expect_ok and proc.returncode != 0:
            self.fail(f"{' '.join(cmd)} exited {proc.returncode}:\n"
                      f"{out}{proc.stderr.decode(errors='replace')}")
        return out

    def lines(self, script, *flags, **kw):
        return self.run(script, *flags, **kw).splitlines()

    def clear_caches(self):
        for root, _, files in os.walk(self.dir):
            for f in files:
                if f.startswith('.') and f.endswith('.roc'):
                    os.remove(os.path.join(root, f))

    def cache_path(self, module, debug=True):
        return os.path.join(self.dir, f'.{module}{"" if debug else ".nodebug"}.roc')

    def cache_size(self, module, debug=True):
        path = self.cache_path(module, debug)
        if not os.path.exists(path):
            self.fail(f'no cache file {path}')
        return os.path.getsize(path)

    def edit(self, name, old, new):
        path = os.path.join(self.dir, name)
        with open(path, encoding='utf-8') as f:
            text = f.read()
        if old not in text:
            self.fail(f'{name} does not contain {old!r}')
        # A different stamp is guaranteed by the size change; mtime need not tick.
        with open(path, 'w', encoding='utf-8') as f:
            f.write(text.replace(old, new))

    def expect(self, what, got, want):
        if got != want:
            self.fail(f'{what}: got {got!r}, want {want!r}')

    def fail(self, message):
        raise Scenario.Failed(message)


# ---------------------------------------------------------------------------
# The chain used by several scenarios: main -> a -> b -> c, plus main2 (c
# directly and through a) and main3 (c first, then a; with module state).

def chain(s):
    s.module(PFX + 'c', funcs=40, extra='var count = 0\nfunc bump():\n  count = count + 1\n  return count')
    s.module(PFX + 'b', imports=[PFX + 'c'])
    s.module(PFX + 'a', imports=[PFX + 'b'])
    a, b, c = PFX + 'a', PFX + 'b', PFX + 'c'
    s.write('main.rox', f"import {a}\nprint('main: ' + {a}.{b}.{c}.{c}_ver())\n")
    s.write('main2.rox', f"import {c}\nimport {a}\nprint('direct: ' + {c}.{c}_ver())\n"
                         f"print('via a: ' + {a}.{b}.{c}.{c}_ver())\n")
    s.write('main3.rox', f"import {c}\nimport {a}\nprint('direct ' + string({c}.bump()))\n"
                         f"print('via a  ' + string({a}.{b}.{c}.bump()))\n")


def edit_leaf(s):
    """Editing a leaf module is seen by an unchanged importer on the next run."""
    chain(s)
    s.expect('cold', s.lines('main.rox')[-1], f'main: {PFX}c-v1')
    s.edit(PFX + 'c.rox', f"'{PFX}c-v1'", f"'{PFX}c-v2'")
    s.expect('after editing c', s.lines('main.rox')[-1], f'main: {PFX}c-v2')
    s.expect('warm again', s.lines('main.rox')[-1], f'main: {PFX}c-v2')


def import_order(s):
    """Caches written while c was already loaded (main3) serve main, which
    reaches c only through a."""
    chain(s)
    s.run('main3.rox')
    s.expect('main from main3-built caches', s.lines('main.rox')[-1], f'main: {PFX}c-v1')


def body_once(s):
    """A module body runs exactly once per program: cold, warm, and mixed
    caches, with c imported directly and through a."""
    chain(s)
    s.run('main.rox')                          # caches built with c reached only through a
    os.utime(os.path.join(s.dir, PFX + 'c.rox'))   # c recompiles; a's cache must not carry a copy
    for state in ('touched c', 'warm'):
        out = s.lines('main2.rox')
        s.expect(f'{state} body count', out.count(f'body {PFX}c'), 1)
        s.expect(f'{state} result', out[-2:], [f'direct: {PFX}c-v1', f'via a: {PFX}c-v1'])
    os.remove(s.cache_path(PFX + 'b'))
    out = s.lines('main2.rox')
    s.expect('mixed body count', out.count(f'body {PFX}c'), 1)
    s.expect('mixed order', [l for l in out if l.startswith('body')],
             [f'body {PFX}c', f'body {PFX}b', f'body {PFX}a'])


def shared_state(s):
    """Module state is one object however the module is reached."""
    chain(s)
    for state in ('cold', 'warm'):
        s.expect(f'{state}', s.lines('main3.rox')[-2:], ['direct 1', 'via a  2'])


def sizes(s):
    """Each .roc holds its own module only: an importer is not the sum of its
    subtree, and importing a builtin costs bytes, not its member table."""
    chain(s)
    s.run('main.rox')
    c = s.cache_size(PFX + 'c')
    a = s.cache_size(PFX + 'a')
    main = s.cache_size('main')
    if a > c / 2:
        s.fail(f'.{PFX}a.roc is {a} bytes; c alone is {c}: the importer embeds its subtree')
    if main > 4096:
        s.fail(f'.main.roc is {main} bytes for a two-line script')
    s.write('m.rox', 'import math\nprint(1)\n')
    s.run('m.rox')
    m = s.cache_size('m')
    if m > 4096:
        s.fail(f'.m.roc is {m} bytes for `import math`: the builtin was snapshotted')


def circular(s):
    """Mutual imports behave the same with no cache, cold, warm, and from a
    cache built through the other entry point."""
    ca, cb = PFX + 'ca', PFX + 'cb'
    s.write(ca + '.rox', f"import {cb}\nprint('body {ca}')\nfunc fa():\n  return 'fa'\nfunc ga():\n  return {cb}.fb()\n")
    s.write(cb + '.rox', f"import {ca}\nprint('body {cb}')\nfunc fb():\n  return 'fb'\nfunc gb():\n  return {ca}.fa()\n")
    s.write('main.rox', f"import {ca}\nprint({ca}.ga())\nprint({ca}.{cb}.gb())\n")
    s.write('main2.rox', f"import {cb}\nprint({cb}.gb())\n")
    ref = s.lines('main.rox', '--nocache')
    ref2 = s.lines('main2.rox', '--nocache')
    s.expect('main cold', s.lines('main.rox'), ref)
    s.expect('main warm', s.lines('main.rox'), ref)
    s.expect('main2 from main-built caches', s.lines('main2.rox'), ref2)
    s.expect('main2 warm', s.lines('main2.rox'), ref2)


def older_copy(s):
    """A source replaced by an older-mtime copy (cp -p, rsync -a) is detected."""
    chain(s)
    old = os.path.join(s.dir, PFX + 'c.rox.bak')
    shutil.copy2(os.path.join(s.dir, PFX + 'c.rox'), old)
    s.run('main.rox')
    s.edit(PFX + 'c.rox', f"'{PFX}c-v1'", f"'{PFX}c-v2'")
    s.expect('edited', s.lines('main.rox')[-1], f'main: {PFX}c-v2')
    shutil.copy2(old, os.path.join(s.dir, PFX + 'c.rox'))   # older mtime than the caches
    s.expect('older copy restored', s.lines('main.rox')[-1], f'main: {PFX}c-v1')


def suffix_cached(s):
    """A user module's @suffix functions survive its cache."""
    u = PFX + 'u'
    s.write(u + '.rox', '@suffix("zz")\nfunc zz(x):\n  return x * 2\n')
    s.write('main.rox', f'import {u}.*\nprint(3zz)\n')
    s.expect('cold', s.lines('main.rox'), ['6'])
    os.remove(s.cache_path('main'))           # u from its cache, main recompiles
    s.expect('u cached, main recompiled', s.lines('main.rox'), ['6'])
    s.expect('warm', s.lines('main.rox'), ['6'])


def packages(s):
    """Package init.rox runs first, once, in every import order and cache state."""
    pkg = PFX + 'pkg'
    os.mkdir(os.path.join(s.dir, pkg))
    s.write(f'{pkg}/init.rox', "print('pkg init')\nfunc top():\n  return 'top'\n")
    s.write(f'{pkg}/sub.rox', "print('sub body')\nfunc s():\n  return 's'\n")
    s.write(f'{pkg}/other.rox', "print('other body')\nfunc o():\n  return 'o'\n")
    s.write('usesub.rox', f'import {pkg}.sub\nfunc x():\n  return {pkg}.sub.s()\n')
    s.write('main.rox', f"import {pkg}.other\nimport usesub\nprint(usesub.x())\nprint({pkg}.other.o())\n")
    s.write('main2.rox', f"import {pkg}\nimport {pkg}.sub\nprint({pkg}.top())\nprint({pkg}.sub.s())\n")
    s.write('main3.rox', f"import {pkg}.sub\nimport {pkg}\nprint({pkg}.top())\nprint({pkg}.sub.s())\n")
    want = {
        'main.rox': ['pkg init', 'other body', 'sub body', 's', 'o'],
        'main2.rox': ['pkg init', 'sub body', 'top', 's'],
        'main3.rox': ['pkg init', 'sub body', 'top', 's'],
    }
    for script, lines in want.items():
        s.expect(f'{script} nocache', s.lines(script, '--nocache'), lines)
        s.expect(f'{script} cold', s.lines(script), lines)
        s.expect(f'{script} warm', s.lines(script), lines)


def debug_tier(s):
    """Stripped and debug caches are separate files; each run uses its own."""
    chain(s)
    s.expect('nodebug', s.lines('main.rox', '--no-debug-info')[-1], f'main: {PFX}c-v1')
    for m in (PFX + 'a', PFX + 'c', 'main'):
        s.cache_size(m, debug=False)
        if os.path.exists(s.cache_path(m, debug=True)):
            s.fail(f'--no-debug-info wrote a debug-tier cache for {m}')
    s.expect('debug', s.lines('main.rox')[-1], f'main: {PFX}c-v1')
    for m in (PFX + 'a', PFX + 'c', 'main'):
        s.cache_size(m, debug=True)


def precompile(s):
    """--precompile writes every cache; the run then loads them all."""
    chain(s)
    s.run('main.rox', '--precompile')
    for m in (PFX + 'a', PFX + 'b', PFX + 'c', 'main'):
        s.cache_size(m)
    out = s.lines('main.rox')
    s.expect('bodies', [l for l in out if l.startswith('body')],
             [f'body {PFX}c', f'body {PFX}b', f'body {PFX}a'])
    s.expect('result', out[-1], f'main: {PFX}c-v1')


def idl_import(s):
    """An .idl import is restored from cache whether it is direct (before or
    after a user import) or reached through an imported module."""
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'HelloWorldData.idl')
    shutil.copy(src, os.path.join(s.dir, 'HelloWorldData.idl'))
    h = PFX + 'helper'
    s.write(h + '.rox', "import HelloWorldData\nfunc make():\n  var m = HelloWorldData.Msg()\n"
                        "  m.message = 'via helper'\n  return m\nfunc hi():\n  return 'hi'\n")
    s.write('direct.rox', f"import HelloWorldData\nimport {h}\nvar m = HelloWorldData.Msg()\n"
                          f"m.message = 'x'\nprint(m.message)\nprint({h}.hi())\n")
    s.write('through.rox', f'import {h}\nprint({h}.make().message)\n')
    for script, lines in (('direct.rox', ['x', 'hi']), ('through.rox', ['via helper'])):
        s.expect(f'{script} cold', s.lines(script), lines)
        s.expect(f'{script} warm', s.lines(script), lines)


def search_path_change(s):
    """A dependency of a dependency resolving to a different file (another
    search root) invalidates the importer, whose bytecode assumed the old
    interface: `q` is a module global with the first base, an inherited
    property with the second."""
    base, mid = PFX + 'base', PFX + 'mid'
    for root, body in (('r1', 'type Base object:\n  var p = 1\n'),
                       ('r2', 'type Base object:\n  var p = 1\n  var q = 2\n')):
        os.mkdir(os.path.join(s.dir, root))
        s.write(f'{root}/{base}.rox', body)
    s.write(mid + '.rox', f'import {base}\ntype Mid object extends {base}.Base:\n  func mp():\n    return p\n')
    s.write('main.rox', f'import {mid}\nvar q = 100\ntype M object extends {mid}.Mid:\n'
                        f'  func get():\n    return q\nprint(M().get())\n')
    r1 = {'ROXALPATH': s.dir + os.pathsep + os.path.join(s.dir, 'r1')}
    r2 = {'ROXALPATH': s.dir + os.pathsep + os.path.join(s.dir, 'r2')}
    s.expect('r1 cold', s.lines('main.rox', env=r1), ['100'])
    s.expect('r1 warm', s.lines('main.rox', env=r1), ['100'])
    s.expect('r2 (base resolves elsewhere)', s.lines('main.rox', env=r2), ['2'])
    s.expect('r2 warm', s.lines('main.rox', env=r2), ['2'])
    s.expect('back to r1', s.lines('main.rox', env=r1), ['100'])


def package_init_added(s):
    """An init.rox appearing in (or vanishing from) a package folder that was
    cached as a plain namespace is noticed."""
    pkg = PFX + 'pkg'
    os.mkdir(os.path.join(s.dir, pkg))
    s.write(f'{pkg}/sub.rox', "print('sub body')\nfunc s():\n  return 's'\n")
    s.write('main.rox', f'import {pkg}.sub\nprint({pkg}.sub.s())\n')
    s.expect('cold', s.lines('main.rox'), ['sub body', 's'])
    s.expect('warm', s.lines('main.rox'), ['sub body', 's'])
    s.write(f'{pkg}/init.rox', "print('pkg init')\n")
    s.expect('init.rox added', s.lines('main.rox'), ['pkg init', 'sub body', 's'])
    s.expect('init.rox added, warm', s.lines('main.rox'), ['pkg init', 'sub body', 's'])
    os.remove(os.path.join(s.dir, pkg, 'init.rox'))
    s.expect('init.rox removed', s.lines('main.rox'), ['sub body', 's'])


def promoted_dependency(s):
    """A module reached transitively through a cached dependency and then
    imported directly is linkable: the importer's cache is written."""
    chain(s)
    s.run('main.rox')                          # a, b, c cached; c is transitive for a
    c = PFX + 'c'
    s.write('main4.rox', f'import {PFX}a\nimport {c}\nprint({c}.{c}_ver())\n')
    s.expect('cold', s.lines('main4.rox')[-1], f'{c}-v1')
    s.cache_size('main4')                      # fails if the write was abandoned
    s.expect('warm', s.lines('main4.rox')[-1], f'{c}-v1')


SCENARIOS = {
    'edit_leaf': (edit_leaf, set()),
    'import_order': (import_order, set()),
    'body_once': (body_once, set()),
    'shared_state': (shared_state, set()),
    'sizes': (sizes, set()),
    'circular': (circular, set()),
    'older_copy': (older_copy, set()),
    'suffix_cached': (suffix_cached, set()),
    'packages': (packages, set()),
    'debug_tier': (debug_tier, set()),
    'precompile': (precompile, set()),
    'idl_import': (idl_import, {'dds'}),
    'search_path_change': (search_path_change, set()),
    'package_init_added': (package_init_added, set()),
    'promoted_dependency': (promoted_dependency, set()),
}


def scenario_names(features=None):
    """Names whose feature requirements `features` (a set) satisfies."""
    return [name for name, (_, needs) in SCENARIOS.items()
            if features is None or needs <= set(features)]


def run(name, roxal, env, timeout):
    """(passed, detail) for one scenario."""
    fn, _ = SCENARIOS[name]
    s = Scenario(os.path.abspath(roxal), env, timeout)
    try:
        fn(s)
        return True, ''
    except Scenario.Failed as e:
        return False, f'{name}: {e}'
    except subprocess.TimeoutExpired as e:
        return False, f'{name}: timeout after {timeout} s: {e.cmd}'
    finally:
        s.cleanup()


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    names = sys.argv[2:] or scenario_names()
    failed = 0
    for n in names:
        ok, detail = run(n, sys.argv[1], os.environ, 60)
        print(f'{n:<16} {"pass" if ok else "FAIL: " + detail}')
        failed += not ok
    sys.exit(1 if failed else 0)
