#!/usr/bin/env python3

import os
import subprocess
import argparse
import re
import time

# Parse command-line arguments
parser = argparse.ArgumentParser(description="Run Roxal tests.")
parser.add_argument('--convs', action='store_true', help='Include tests/conversions/* tests')
args = parser.parse_args()

# for each named test, run the <test>.rox file in the tests folder
# and compare its output with <test>.out (stdout) and <test>.err (stderr regex)

tests = [
    'comments', 'primitive1', 'constants', 'scopetest2', 'scopetest3',
    'andtest', 'ortest', 'not',
    'arith', 'factorial', 'defaultvalues', 'typeof_test',
    'dict', 'dict2', 'dict_keyerror', 'list', 'list2', 'list_add_test', 'range', 'range2', 'enum1', 'enum2', 'enum3', 'upvalue_leak',
    'unicode', 'signal_clock', 'signal_func_nocall', 'signal_func_exec', 'signal_index', 'signal_on_stmt', 'signal_on_threads', 'on_expression',
    'test_signal_value_property', 'construct_by_signal', 'signal_run_stop', 'signal_network1',
    'dataflow_clocktest1',
    'event1', 'event_on_stmt', 'event_emit_keyword', 'event_on_method', 'event_ref', 'event_actor_ref', 'event_actor_ref2', 'event_actor_ref3', 'event_actor_ref4',
    'event_in_sleep', 'event_in_sleep2',
    'nonstrict-assign', 'nonstrict-assign-err', 'strict-assign', 'strict-assign-err',
    'module_strict_assign_err', 'func_nonstrict', 'conversions1',
    'byteops',
    'closure', 'closure2', 'closure3', 'closure4', 'closure5', 'lambda1',
    'conversion1',
    'call_param_type_nonstrict', 'call_param_type_strict', 'param_assign_static_err',
    'linkedlist', 'structbindassign',
    'if', 'for1',
    'func_param_default', 'func_param_default2', 'func_param_default3','func_param_default4',
    'typeobj1', 'typeobj2', 'typeobj3', 'typeobj4', 'typeobj5', 'typeobj6', 'typeobj7', 'virtual_method',
    'implements1', 'object_inherit_bank',
    'importmodule1', 'importstar', 'importsyms', 'importdiamond',
    'method_named_param',
    'annot1', 'generic', 'objscopes',
    'threads1', 'fork_upvalue_error', 'fork_no_upvalues', 'actor1', 'actor2', 'actor3', 'actor4', 'actor5', 'actor6', 'actor7', 'actor8', 'actor9', 'actor_init',
    'clone1', 'extends1', 'nothis', 'superprop', 'scopetest4',
    'private_prop', 'private_method', 'private_inherit',
    'typededucer_binop', 'typededucer_ops',
    'mathfuncs',
    'typeassign1', 'typeassign2', 'typeassign3',
    'vector1', 'vector2', 'vector3', 'vector4', 'vector5','vector_methods', 'vector_equal', 'vector_matrix_equal',
    'matrix1', 'matrix2', 'matrix_literal1', 'matrix_literal_newline', 'vector_matrix_negative', 'matrix_index', 'matrix_methods', 'matrix_assign', 'matrix_equal', 'matrix_math', 'ffi1', 'cstruct1', 'cstruct2', 'cstruct3'
    , 'weakref', 'strongref', 'is_operator'
    , 'runtime_error_snippet'
]

# implementation doesn't yet allow these tests to pass
failing_tests = ['signal_network1']


if args.convs:
    conv_test_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tests/conversions')
    conv_tests = sorted([
        os.path.join('conversions', os.path.splitext(f)[0])
        for f in os.listdir(conv_test_dir)
        if f.endswith('.rox') and ('decimal' not in f) and os.path.exists(os.path.join(conv_test_dir, os.path.splitext(f)[0] + '.out'))
    ])
    tests += conv_tests

project_root = os.path.dirname(os.path.abspath(__file__))
test_dir = os.path.join(project_root, 'tests')

roxalpath = 'build'
roxal = './roxal'

# Ensure the FFI test shared library is built
testlib_c = os.path.join(test_dir, 'testlib.c')
testlib_so = os.path.join(test_dir, 'testlib.so')
if os.path.exists(testlib_c):
    if (not os.path.exists(testlib_so) or
            os.path.getmtime(testlib_so) < os.path.getmtime(testlib_c)):
        try:
            subprocess.check_call(
                ['gcc', '-shared', '-fPIC', '-o', testlib_so, testlib_c])
        except Exception as e:
            print('Failed to build testlib.so:', e)
        if os.path.exists(testlib_so):
            print('Built testlib.so')

    if not os.path.exists(testlib_so):
        raise 'testlib.so was not built'


# Track how many tests pass or fail
passed_count = 0
failed_count = 0

cwd = os.getcwd()
os.chdir(os.path.join(project_root, roxalpath))

total_start_time = time.perf_counter()

try:
    for test in tests:
        print(f"Test {test} ", end='', flush=True)
        start_time = time.perf_counter()
        testrox = os.path.join(test_dir, test + '.rox')
        testout = os.path.join(test_dir, test + '.out')
        testerr = os.path.join(test_dir, test + '.err')
        if not os.path.exists(testrox):
            raise RuntimeError(f"Test {testrox} not found.")

        if not (os.path.exists(testout) or os.path.exists(testerr)):
            raise RuntimeError(f"Test expected output {testout} or {testerr} not found.")

        cmd = [roxal, testrox]
        if test.startswith('typededucer_'):
            cmd = [roxal, '--ast', testrox]
        compProc = subprocess.run(cmd, capture_output=True, shell=False)
        duration_ms = (time.perf_counter() - start_time) * 1000

        opt_expected = (" [expected]" if test in failing_tests else '')

        passed = True
        expect_err = os.path.exists(testerr)
        if compProc.returncode != 0 and not expect_err:
            print(f"FAIL: {opt_expected}", flush=True)
            print(f"-- return code {compProc.returncode} --")
            if compProc.returncode < 0:
                import signal
                signum = -compProc.returncode
                try:
                    sig_name = signal.Signals(signum).name
                except ValueError:
                    sig_name = str(signum)
                print(f"Process terminated by signal: {sig_name}")
            print()
            passed = False

        crash_output = (b'segmentation fault' in compProc.stdout.lower() or
                        b'segmentation fault' in compProc.stderr.lower() or
                        b'abort' in compProc.stdout.lower() or
                        b'abort' in compProc.stderr.lower())
        if crash_output and passed:
            print(f"FAIL: {opt_expected}", flush=True)
            print("-- abnormal termination message detected --")
            print(compProc.stdout)
            print(compProc.stderr)
            print("--")
            passed = False
        if os.path.exists(testout):
            with open(testout, mode='rb') as file:
                expected = file.read()
            if expected != compProc.stdout:
                print(f"FAIL: {opt_expected}", flush=True)
                print("-- stdout --")
                print(compProc.stdout)
                print("-- expected stdout --")
                print(expected)
                print("--")
                print()
                passed = False
        if os.path.exists(testerr):
            with open(testerr, 'r') as file:
                err_re = file.read().strip()
            stderr_str = compProc.stderr.decode()
            if re.search(err_re, stderr_str, re.MULTILINE | re.DOTALL) is None:
                print(f"FAIL: {opt_expected}", flush=True)
                print("-- stderr --")
                print(stderr_str)
                print("-- expected regex --")
                print(err_re)
                print("--")
                print()
                passed = False
        if passed:
            print(f"pass ({duration_ms:.0f} ms)", flush=True)
            passed_count += 1
        else:
            print(f"({duration_ms:.1f} ms)", flush=True)
            failed_count += 1

except Exception as e:
    print('Exception: ' + str(e))

total_duration = time.perf_counter() - total_start_time
print(f"{passed_count} tests passed, {failed_count} failed in {total_duration:.2f} s")
if failed_count > 0:
  print(f"Tests expecied to fail currently: {', '.join(failing_tests)}")

os.chdir(cwd)
