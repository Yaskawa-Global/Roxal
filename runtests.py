#!/usr/bin/env python3

import os
import subprocess
import argparse
import re
import time

# Maximum time in seconds to allow each test to run
TEST_TIMEOUT_SECS = 5
GC_STRESS_TIMEOUT_SECS = 20
# Width of the test name column when printing results
TEST_NAME_WIDTH = 32

# Parse command-line arguments
parser = argparse.ArgumentParser(description="Run Roxal tests.")
parser.add_argument('--convs', action='store_true', help='Include tests/conversions/* tests')
parser.add_argument('--all', action='store_true', help='Run all tests, including conversions and long running tests')
parser.add_argument('--opcode-prof', action='store_true', help='Enable opcode profiling for each Roxal invocation')
parser.add_argument('--nocache', action='store_true', help='Disable reading and writing Roxal bytecode cache files')
parser.add_argument('--nogc', action='store_true', help='Disable Roxal garbage collection during tests')
parser.add_argument('--recompile', action='store_true', help='Force Roxal to recompile input scripts on each run')
args = parser.parse_args()


def is_debug_build(build_dir: str) -> bool:
    flags_path = os.path.join(build_dir, 'CMakeFiles', 'roxal.dir', 'flags.make')
    try:
        with open(flags_path, 'r', encoding='utf-8') as handle:
            contents = handle.read()
        return 'DEBUG_BUILD' in contents
    except OSError:
        return False

# for each named test, run the <test>.rox file in the tests folder
# and compare its output with <test>.out (stdout) and <test>.err (stderr regex)

tests = [
    'comments', 'primitive1', 'constants', 'scopetest2', 'scopetest3',
    'andtest', 'ortest', 'not',
    'arith', 'factorial', 'defaultvalues', 'construct_defaults', 'typeof_test',
    'dict', 'dict2', 'dict_keyerror', 'dict_dot', 'dict_dot_keyerror', 'dict_self_reference', 'list', 'list2', 'list_negative_index', 'list_self_reference', 'copyinto_list', 'copyinto_list_unicode', 'copyinto_sublist', 'copyinto_signal',
    'list_add_test', 'list_dict_equal', 'range', 'range2', 'enum1', 'enum2', 'enum3', 'upvalue_leak',
    'unicode', 'signal_clock', 'signal_add', 'signal_subtract', 'signal_multiply', 'signal_divide', 'signal_modulo',
    'signal_greater', 'signal_less', 'signal_equal', 'signal_history', 'signal_cycle', 'signal_cleanup',
    'signal_and', 'signal_or', 'signal_not', 'signal_band', 'signal_bor', 'signal_bxor', 'signal_bnot',
    'signal_func_nocall', 'signal_func_exec', 'signal_index', 'signal_on_stmt', 'signal_on_threads', 'signal_on_in_method', 'on_expression',
    'test_signal_value_property', 'test_signal_name_property', 'signal_named_param', 'construct_by_signal', 'signal_run_stop', 'signal_source', 'signal_default_err', 'signal_network1',
    'dataflow_clocktest1', 'multi_clock', 'clock_error', 'clock_name_param',
    'event1', 'event_on_stmt', 'event_emit_keyword', 'event_on_method', 'event_ref', 'event_actor_ref', 'event_actor_ref2', 'event_actor_ref3', 'event_actor_ref4', 'event_instance_emit', 'event_payload', 'event_implicit_constructor', 'event_type_on',
    'event_in_sleep', 'event_in_sleep2',
    'until_event', 'until_signal', 'signal_vector_dot',
    'nonstrict-assign', 'nonstrict-assign-err', 'strict-assign', 'strict-assign-err',
    'module_strict_assign_err', 'var_redeclare_err', 'var_redeclare_assign_err', 'repl_var_redeclare_err', 'func_nonstrict', 'conversions1',
    'serialize_values', 'serialize_objects', 'serialize_user_objects', 'serialize_func', 'serialize_actor',
    'json_basic',
    'byteops', 'bitwise', 'byte_int_bits', 'list_byte_concat', 'list_enum_concat',
    'object_init', 'object_constructor_args', 'object_constructor_unknown_arg', 'object_constructor_arg_count',
    'object_inherit_is',
    'closure', 'closure2', 'closure3', 'closure4', 'closure5', 'closure_many', 'lambda1',
    'conversion1', 'string_interp',
    'call_param_type_nonstrict', 'call_param_type_strict', 'param_assign_static_err',
    'linkedlist', 'structbindassign',
    'if', 'for1', 'nested_for',
    'func_param_default', 'func_param_default2', 'func_param_default3','func_param_default4',
    'typeobj1', 'typeobj2', 'typeobj3', 'typeobj4', 'typeobj5', 'typeobj6', 'typeobj7',
    'object_to_dict_private', 'object_from_dict', 'virtual_method',
    'implements1', 'object_inherit_bank',
    'importmodule1', 'importstar', 'importsyms', 'importdiamond', 'pkg1/main',
    'import_folder_init', 'import_folder_single',
    'method_named_param',
    'annot1', 'generic', 'objscopes',
    'threads1', 'fork_upvalue_error', 'fork_no_upvalues',
    'actor1', 'actor2', 'actor3', 'actor4', 'actor5', 'actor6', 'actor7', 'actor8', 'actor9',
    'actor_init', 'actor_stack', 'actor_future', 'future_ready',
    'actor_closure1', 'actor_closure2', 'actor_closure3',
    'clone1', 'extends1', 'nothis', 'superprop', 'scopetest4',
    'private_prop', 'private_method', 'private_inherit',
    'typededucer_binop', 'typededucer_ops', 'typededucer_until', 'typededucer_bitwise',
    'time_basic', 'mathfuncs',
    'typeassign1', 'typeassign2', 'typeassign3',
    'vector1', 'vector2', 'vector3', 'vector4', 'vector5','vector_methods', 'vector_equal', 'vector_matrix_equal',
    'matrix1', 'matrix2', 'matrix_literal1', 'matrix_literal_newline', 'vector_matrix_negative', 'unary_vector_matrix',
    'matrix_index', 'matrix_methods', 'matrix_assign', 'matrix_equal', 'matrix_math',
    'ffi1', 'ffi_addfloats', 'ffi_struct_out', 'ffi_inttypes', 'ffi_strlen', 'ffi_relative', 'ffi_toupper', 'ffi_primptr', 'ffi_voidptr_struct', 'cstruct1', 'cstruct2', 'cstruct3', 'cstruct_byval', 'cstruct_array',
    'nested_cstruct', 'nested_cstruct_ptr', 'nested_cstruct_byval',
    'weakref', 'strongref', 'is_operator', 'stackdepth', 'modulevar2',
    'const_basic', 'const_assign_err', 'const_nonliteral_err', 'const_missing_initializer_err',
    'const_property', 'const_property_method_err', 'const_property_runtime_err', 'const_module_assign',
    'actor_module_const', 'actor_module_var_err',
    'is_operator_type',
    'runtime_error_snippet', 'exception_basic', 'exception_typed', 'exception_rethrow', 'exception_string',
    'stacktrace', 'exception_stacktrace', 'object_user_ref_cycle', 'gc_list_cycle', 'gc_liveness',
    'runtime_error_snippet',
    'property_count', 'cmdline_execute', 'repl_run', 'invalid_option', 'fileio_basic', 'fileio_binary',
    'fileio_read_binary', 'fileio_write_binary', 'fileio_actor_write', 'fileio_delete', 'fileio_extra',
    'help_doc', 'help_time_wall_now', 'help_time_wall_now_instance', 'docstring_func',
    'builtin_object_methods', 'math_counter_signal', 'print_flush'
]

long_running_tests = [
    'gc_stress',
]

# implementation doesn't yet allow these tests to pass (do not add to this list without human consent)
failing_tests = ['signal_network1']
assert(set(failing_tests).issubset(set(tests) | set(long_running_tests)))


include_convs = args.convs or args.all
if include_convs:
    conv_test_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tests/conversions')
    conv_tests = sorted([
        os.path.join('conversions', os.path.splitext(f)[0])
        for f in os.listdir(conv_test_dir)
        if f.endswith('.rox') and ('decimal' not in f) and os.path.exists(os.path.join(conv_test_dir, os.path.splitext(f)[0] + '.out'))
    ])
    tests += conv_tests

if args.all:
    tests += long_running_tests

project_root = os.path.dirname(os.path.abspath(__file__))
test_dir = os.path.join(project_root, 'tests')

roxalpath = 'build'
roxal = './roxal'

build_dir = os.path.join(project_root, roxalpath)
if args.opcode_prof and not is_debug_build(build_dir):
    raise SystemExit("--opcode-prof requires a Debug build (configure CMake with -DCMAKE_BUILD_TYPE=Debug).")

opcode_profile_path = os.path.abspath(os.path.join(build_dir, 'opcode_profile.json'))

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

env_base = os.environ.copy()
env_base['ROXALPATH'] = test_dir

total_start_time = time.perf_counter()

try:
    for test in tests:
        print(f"Test {test:<{TEST_NAME_WIDTH}} ", end='', flush=True)
        start_time = time.perf_counter()
        testrox = os.path.join(test_dir, test + '.rox')
        testout = os.path.join(test_dir, test + '.out')
        testerr = os.path.join(test_dir, test + '.err')
        if not os.path.exists(testrox):
            raise RuntimeError(f"Test {testrox} not found.")

        if not (os.path.exists(testout) or os.path.exists(testerr)):
            raise RuntimeError(f"Test expected output {testout} or {testerr} not found.")

        rel_testrox = os.path.relpath(testrox, os.getcwd())
        input_data = None
        cmd = [roxal, rel_testrox]
        if test.startswith('repl_'):
            with open(testrox, 'r') as f:
                input_data = f.read()
            cmd = [roxal]
        elif test.startswith('typededucer_'):
            cmd = [roxal, '--ast', rel_testrox]
        if test == 'cmdline_execute':
            with open(testrox, 'r') as f:
                snippet = f.read().strip()
            cmd = [roxal, '-e', snippet]
        if test == 'repl_run':
            script_path = os.path.join(test_dir, 'repl_run_script.rox')
            rel_script = os.path.relpath(script_path, os.getcwd())
            cmd = [roxal]
            input_data = f"run {rel_script}\nquit\n".encode()
        if test == 'invalid_option':
            cmd = [roxal, '--bogus']

        if args.opcode_prof and '--opcode-prof' not in cmd:
            cmd = [cmd[0], '--opcode-prof', *cmd[1:]]
        if args.nocache and '--nocache' not in cmd:
            cmd = [cmd[0], '--nocache', *cmd[1:]]
        if args.nogc and '--nogc' not in cmd:
            cmd = [cmd[0], '--nogc', *cmd[1:]]
        if args.recompile and '--recompile' not in cmd:
            cmd = [cmd[0], '--recompile', *cmd[1:]]

        opt_expected = (" [expected]" if test in failing_tests else '')

        timeout_secs = GC_STRESS_TIMEOUT_SECS if test == 'gc_stress' else TEST_TIMEOUT_SECS

        try:
            compProc = subprocess.run(
                cmd,
                input=(input_data.encode() if isinstance(input_data, str) else input_data if input_data else None),
                capture_output=True, shell=False,
                timeout=timeout_secs, env=env_base)
        except subprocess.TimeoutExpired:
            duration_ms = (time.perf_counter() - start_time) * 1000
            print(f"FAIL: {opt_expected}", flush=True)
            print(f"-- timeout after {timeout_secs} s --")
            print()
            failed_count += 1
            continue
        duration_ms = (time.perf_counter() - start_time) * 1000


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
failed_unexpected_count = failed_count - len(failing_tests)
print()
print(f"{passed_count} tests passed, {failed_unexpected_count} "+('FAILED' if failed_unexpected_count>0 else 'failed')+f" unexpectedly ({len(failing_tests)} were expected to fail)")
if failed_count > 0:
  print(f"Tests expecied to fail currently: {', '.join(failing_tests)}")
print(f"Total time {total_duration:.2f} s")

if args.opcode_prof:
    if os.path.exists(opcode_profile_path):
        print(f"Opcode profile written to {opcode_profile_path}")
    else:
        print(f"Opcode profiling was requested but {opcode_profile_path} was not created.")

os.chdir(cwd)
