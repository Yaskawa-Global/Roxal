#!/usr/bin/env python3

from __future__ import annotations
import os
import sys
import socket
import subprocess
import argparse
import fnmatch
import re
import time
import tempfile
from typing import Set

# Maximum time in seconds to allow each test to run
TEST_TIMEOUT_SECS = 5
GC_STRESS_TIMEOUT_SECS = 20
NN_LFS_TIMEOUT_SECS = 60
# Width of the test name column when printing results
TEST_NAME_WIDTH = 32
GRPC_TEST_ADDR = "127.0.0.1:50051"
COMPUTE_TEST_ADDR = "127.0.0.1:56925"
COMPUTE_TEST_ADDR_2 = "127.0.0.1:56926"
COMPUTE_TEST_ADDR_PLACEHOLDER = "__COMPUTE_TEST_ADDR__"
COMPUTE_TEST_ADDR_2_PLACEHOLDER = "__COMPUTE_TEST_ADDR_2__"


def read_compute_protocol_version(project_root: str) -> int:
    header_path = os.path.join(project_root, 'compiler', 'ComputeProtocol.h')
    with open(header_path, 'r', encoding='utf-8') as handle:
        header = handle.read()
    match = re.search(r'ComputeVersion\s*=\s*(\d+)', header)
    if not match:
        raise RuntimeError(f"Unable to parse ComputeVersion from {header_path}")
    return int(match.group(1))

# Parse command-line arguments
parser = argparse.ArgumentParser(description="Run Roxal tests.")
parser.add_argument('--convs', action='store_true', help='Include tests/conversions/* tests')
parser.add_argument('--all', action='store_true', help='Run all tests, including conversions and long running tests')
parser.add_argument('--opcode-prof', action='store_true', help='Enable opcode profiling for each Roxal invocation')
parser.add_argument('--nocache', action='store_true', help='Disable reading and writing Roxal bytecode cache files')
parser.add_argument('--nogc', action='store_true', help='Disable Roxal garbage collection during tests')
parser.add_argument('--recompile', action='store_true', help='Delete cached .roc files before running tests')
parser.add_argument('--build', action='store_true', help='Invoke cmake --build before running the tests')
parser.add_argument('--test', '-t', type=str, metavar='PATTERN', help='Only run tests matching PATTERN (shell-style wildcards: * ? [seq])')
args = parser.parse_args()


def detect_features(roxal_binary: str) -> Set[str]:
    """Return feature tags reported by `roxal --version`."""
    try:
        proc = subprocess.run([roxal_binary, '--version'],
                              capture_output=True, text=True, check=False)
    except Exception:
        return set()
    if proc.returncode != 0:
        return set()
    match = re.search(r'\[([^\]]*)\]', proc.stdout)
    if not match:
        return set()
    entries = [fragment.strip() for fragment in match.group(1).split(',')]
    return {entry for entry in entries if entry}


def is_lfs_pointer(path: str) -> bool:
    """Check if a file is a Git LFS pointer rather than actual content."""
    try:
        with open(path, 'rb') as f:
            return f.read(48).startswith(b'version https://git-lfs.github.com/spec/v1')
    except FileNotFoundError:
        return True  # missing file should be treated as unavailable


def is_debug_build(build_dir: str) -> bool:
    flags_path = os.path.join(build_dir, 'CMakeFiles', 'roxal.dir', 'flags.make')
    try:
        with open(flags_path, 'r', encoding='utf-8') as handle:
            contents = handle.read()
        return 'DEBUG_BUILD' in contents
    except OSError:
        return False


def clear_bytecode_cache(root_dir: str) -> int:
    """Delete cached Roxal bytecode files (.*.roc) under root_dir."""
    removed = 0
    for dirpath, _, filenames in os.walk(root_dir):
        for filename in filenames:
            if not (filename.startswith('.') and filename.endswith('.roc')):
                continue
            cache_path = os.path.join(dirpath, filename)
            try:
                os.remove(cache_path)
                removed += 1
            except FileNotFoundError:
                continue
    return removed

# for each named test, run the <test>.rox file in the tests folder
# and compare its output with <test>.out (stdout) and <test>.err (stderr regex)

tests = [
    'comments', 'primitive1', 'constants', 'scopetest2', 'scopetest3',
    'andtest', 'ortest', 'not', 'not_nil_conversion_err', 'is_not_nil', 'is_not_non_nil',
    'arith', 'factorial', 'defaultvalues', 'construct_defaults', 'typeof_test',
    'dict', 'dict2', 'dict_keyerror', 'dict_dot', 'dict_dot_keyerror', 'dict_self_reference', 'list', 'list2', 'list_negative_index', 'list_self_reference', 'copyinto_list', 'copyinto_list_unicode', 'copyinto_sublist', 'copyinto_signal',
    'list_add_test', 'list_dict_equal', 'test_filter_map_reduce', 'list_method_exception', 'test_paren_continuation', 'range', 'range2', 'enum1', 'enum2', 'enum3', 'upvalue_leak',
    'unicode', 'backtick_identifier', 'signal_clock', 'signal_add', 'signal_subtract', 'signal_multiply', 'signal_divide', 'signal_modulo',
    'signal_greater', 'signal_less', 'signal_equal', 'signal_history', 'signal_cycle', 'signal_cleanup',
    'signal_and', 'signal_or', 'signal_not', 'signal_band', 'signal_bor', 'signal_bxor', 'signal_bnot',
    'signal_func_nocall', 'signal_func_exec', 'signal_index', 'signal_when_stmt', 'signal_when_threads', 'when_expression', 'signal_when_in_method', 'signal_when_becomes', 'signal_on_changed_test',
    'module_var_when_changed', 'module_var_when_changed_string', 'module_var_when_becomes', 'object_member_when_changed', 'when_obj_becomes', 'when_accessor_var_changes',
    'test_signal_value_property', 'test_signal_name_property', 'signal_named_param', 'construct_by_signal', 'signal_run_stop', 'signal_source', 'signal_default_err', 'signal_network1',
    'signal_islands',
    'dataflow_clocktest1', 'multi_clock', 'clock_error', 'clock_name_param',
    'event1', 'event_when_stmt', 'event_emit_keyword', 'event_when_method', 'event_remove_method', 'event_ref', 'event_actor_ref', 'event_actor_ref2', 'event_actor_ref3', 'event_actor_ref4', 'event_instance_emit',
    'event_payload', 'event_implicit_constructor', 'event_type_when', 'event_target_filter',
    'event_in_sleep', 'event_in_sleep2', 'event_cascade',
    'until_event', 'until_signal', 'signal_vector_dot',
    'nonstrict-assign', 'nonstrict-assign-err', 'strict-assign', 'strict-assign-err',
    'module_strict_assign_err', 'var_redeclare_err', 'var_redeclare_assign_err', 'repl_var_redeclare_err', 'func_nonstrict', 'conversions1',
    'serialize_values', 'serialize_signal', 'serialize_objects', 'serialize_user_objects', 'serialize_func', 'serialize_actor',
    'json_basic',
    'byteops', 'bitwise', 'byte_int_bits', 'int64_promotion', 'int64_bounds', 'list_byte_concat', 'list_enum_concat',
    'object_init', 'object_constructor_args', 'object_constructor_unknown_arg', 'object_constructor_arg_count',
    'object_inherit_is', 'object_downcast', 'object_ref_member_default',
    'closure', 'closure2', 'closure3', 'closure4', 'closure5', 'closure_many', 'lambda1', 'lambda2',
    'conversion1', 'string_interp',
    'call_param_type_nonstrict', 'call_param_type_strict', 'param_assign_static_err',
    'linkedlist', 'structbindassign',
    'if', 'for1', 'nested_for',
    'match_simple', 'match_3cases', 'match_repeated', 'match_basic', 'match_enum',
    'with_enum_test', 'with_object_test',
    'func_param_default', 'func_param_default2', 'func_param_default3','func_param_default4',
    'variadic', 'variadic_format', 'variadic_no_comma',
    'typeobj1', 'typeobj2', 'typeobj3', 'typeobj4', 'typeobj5', 'typeobj6', 'typeobj7',
    'object_to_dict_private', 'object_from_dict', 'object_from_dict_set', 'virtual_method',
    'implements1', 'object_inherit_bank',
    'importmodule1', 'importstar', 'importsyms', 'importdiamond', 'pkg1/main',
    'import_return_stack',
    'import_folder_init', 'import_folder_single', 'import_comment_before',
    'method_named_param',
    'annot1', 'generic', 'objscopes',
    'threads1', 'fork_upvalue_error', 'fork_no_upvalues',
    'actor1', 'actor2', 'actor3', 'actor4', 'actor5', 'actor6', 'actor7', 'actor8', 'actor9',
    'actor_init', 'actor_stack', 'actor_future', 'future_ready', 'future_builtin_resolve', 'future_typed_param_resolve', 'wait_duration', 'wait_duration_dim_err', 'wait_duration_mixed_err',
    'actor_method_order',
    'actor_closure1', 'actor_closure2', 'actor_closure3',
    'actor_inter',
    'clone1', 'clone_shared', 'clone_cycle', 'extends1', 'nothis', 'superprop', 'scopetest4', 'local_type_scope',
    'const_member_type_access', 'const_member_type_var_err', 'const_member_type_mutable_err', 'const_member_type_private_err',
    'const_member_freeze', 'const_member_shared', 'const_member_untyped_freeze',
    'nested_type_enum', 'nested_type_object', 'nested_type_inherit',
    'nested_type_sibling', 'nested_type_private', 'nested_type_shadow',
    'dotted_type_name', 'dotted_type_implements', 'dotted_type_deep', 'dotted_type_enum_anno', 'dotted_type_err',
    'private_prop', 'private_method', 'private_inherit',
    'operator_overload', 'operator_overload_cmp', 'operator_overload_commutative',
    'operator_overload_lr', 'operator_overload_inherit', 'operator_overload_fallthrough',
    'operator_overload_proc_err', 'operator_overload_unpaired_err', 'operator_overload_both_err',
    'complex_type',
    'typededucer_binop', 'typededucer_ops', 'typededucer_until', 'typededucer_bitwise',
    'time_basic', 'time_quantity', 'time_quantity_arith',
    'cont_nest_print', 'cont_nest_map', 'cont_nest_filter',
    'cont_nest_map_in_map', 'cont_nest_filter_in_map', 'cont_nest_reduce_in_map',
    'cont_nest_print_in_opstr', 'cont_nest_closure_conv',
    'mathfuncs',
    'typeassign1', 'typeassign2', 'typeassign3',
    'vector1', 'vector2', 'vector3', 'vector4', 'vector5','vector_methods', 'vector_equal', 'vector_matrix_equal',
    'matrix1', 'matrix2', 'matrix_literal1', 'matrix_literal_newline', 'vector_matrix_negative', 'unary_vector_matrix',
    'matrix_index', 'matrix_methods', 'matrix_assign', 'matrix_equal', 'matrix_math',
    'vector_quantity_test', 'orient_test', 'orient_conv_test',
    'tensor_basic', 'tensor_math', 'tensor_compare', 'tensor_convert', 'math_min_max_sum',
    'tensor_convert_err', 'matrix_tensor_err', 'vector_tensor_err',
    'tensor_slice',
    'math_relu', 'math_softmax', 'math_argmax', 'math_clamp',
    'value_semantics', 'value_semantics_cow',
    'ffi1', 'ffi_addfloats', 'ffi_struct_out', 'ffi_inttypes', 'ffi_strlen', 'ffi_relative', 'ffi_toupper', 'ffi_primptr', 'ffi_voidptr_struct', 'cstruct1', 'cstruct2', 'cstruct3', 'cstruct_byval', 'cstruct_array',
    'nested_cstruct', 'nested_cstruct_ptr', 'nested_cstruct_byval',
    'weakref', 'strongref', 'is_operator', 'in_operator', 'stackdepth', 'modulevar2',
    'const_basic', 'const_assign_err', 'const_nonliteral_err', 'const_missing_initializer_err',
    'const_property', 'const_property_method_err', 'const_property_runtime_err', 'const_module_assign',
    'const-interior-mutation',
    'const_list', 'const_dict', 'const_nested', 'const_snapshots', 'const_alias', 'const_identity',
    'const_deep_chain', 'const_cycle', 'const_diamond', 'const_multi_snapshot', 'const_func', 'const_escape_err',
    'const_type_qualifier', 'const_mutable_type', 'const_builtin_method_err', 'const_linked_method_err', 'const_mvcc',
    'const_method_dispatch', 'const_interior_alias',
    'event_const', 'event_const_err', 'event_const_transitive_err',
    'const_signal_err', 'const_signal_type_err',
    'df_capture_mutable_err',
    'actor_const_param', 'actor_const_param_aliased', 'actor_const_param_err',
    'move_local', 'move_module_var', 'move_prop', 'move_const_err', 'move_actor', 'move_zero_copy', 'move_actor_alias_err',
    'move_interior_alias_err',
    'actor_module_const', 'actor_module_var_err',
    'actor_return_mutable_sole', 'actor_return_mutable_shared',
    'actor_return_const_sole', 'actor_return_const_shared',
    'actor_interior_mutate',
    'is_operator_type',
    'runtime_error_snippet', 'exception_basic', 'exception_typed', 'exception_rethrow', 'exception_string',
    'stacktrace', 'exception_stacktrace', 'object_user_ref_cycle', 'gc_list_cycle', 'gc_liveness',
    'runtime_error_snippet',
    'property_count', 'property_accessor', 'property_accessor_oneliner', 'dict_property_getters', 'cmdline_execute', 'repl_run', 'invalid_option', 'fileio_basic', 'fileio_binary',
    'fileio_read_binary', 'fileio_write_binary', 'fileio_actor_write', 'fileio_delete', 'fileio_extra',
    'string_concat_roundtrip', 'actor_concat_stress',
    'help_doc', 'help_wait', 'help_time_wall_now', 'help_time_wall_now_instance', 'docstring_func',
    'builtin_object_methods', 'math_counter_signal', 'print_flush',
    'grpc_message_types', 'grpc_service_actor', 'grpc_int64_values', 'grpc_runtime_error', 'grpc_streaming', 'grpc_args',
    'rt_execution',
    'operator_conv_string', 'operator_conv_string_rettype', 'return_type_conv', 'return_type_conv_upcast',
    'operator_conv_string_inherit',
    'operator_conv_string_implicit',
    'operator_conv_proc_err', 'operator_conv_arity_err', 'operator_conv_rettype_err',
    'operator_conv_object',
    'suffix_basic', 'suffix_braced', 'suffix_compound', 'suffix_string',
    'suffix_unknown_err', 'suffix_edge_cases',
    'quantity_basic',
    'conv_explicit_default',
    'conv_constructor_auto', 'conv_constructor_explicit',
    'conv_func_param_auto',
    'stmt_action_basic', 'stmt_action_chain', 'stmt_action_until',
    'stmt_action_ignore', 'stmt_action_cycle_err',
    'stack_depth_check'
]

grpc_tests = ['grpc_message_types', 'grpc_service_actor', 'grpc_int64_values', 'grpc_runtime_error', 'grpc_streaming', 'grpc_args']
grpc_server_tests = ['grpc_int64_values', 'grpc_streaming', 'grpc_args']
fileio_tests = [
    'fileio_basic', 'fileio_binary', 'fileio_read_binary', 'fileio_write_binary',
    'fileio_actor_write', 'fileio_delete', 'fileio_extra',
    'string_concat_roundtrip', 'actor_concat_stress'
]
dds_tests = ['dds_bounded_ok', 'dds_bounded_fail', 'dds_complex_smoke', 'dds_array_ok', 'dds_array_struct', 'dds_array_multi']
regex_tests = ['regex_test']
xml_tests = [
    'xml_basic_compact', 'xml_basic_raw', 'xml_attrs', 'xml_mixed_raw',
    'xml_compact_lossy', 'xml_whitespace', 'xml_to_xml_compact',
    'xml_to_xml_raw', 'xml_invalid', 'xml_mode_errors',
    'xml_write_mode_error', 'xml_shape_error'
]
socket_tests = ['socket_basic']
nn_tests = ['nn_mnist', 'nn_signal', 'nn_chain', 'nn_signal_chain', 'nn_dynamic', 'nn_multi_io', 'nn_async', 'nn_tokenizer']
nn_lfs_tests = ['nn_dfine']  # require LFS model files (only run with --all)
media_tests = ['media_read_write', 'media_manipulate', 'media_convert']
compute_server_tests = [
    'remote_actor_basic',
    'remote_actor_backchannel',
    'remote_actor_gc_backchannel',
    'remote_actor_gc_idle_retention',
    'remote_actor_gc_inflight',
    'remote_actor_imported_type',
    'remote_actor_forwarded_type',
    'remote_actor_signal_err',
    'remote_actor_tensor',
    'remote_actor_refresh_here',
    'remote_actor_print',
    'remote_actor_print_forwarded',
    'remote_actor_print_here',
    'remote_actor_version_mismatch',
]
compute_server_double_hop_tests = ['remote_actor_forwarded_type', 'remote_actor_print_forwarded']

# Add feature-specific tests to the full list; feature gating happens later.
tests += dds_tests
tests += regex_tests
tests += xml_tests
tests += socket_tests
tests += nn_tests
tests += media_tests
tests += compute_server_tests

long_running_tests = [
    'gc_stress',
    'const_mvcc_stress',
    'rtcallback_test',
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
    tests += nn_lfs_tests

# Filter tests by pattern if --test is specified
if args.test:
    pattern = args.test
    tests = [t for t in tests if fnmatch.fnmatch(t, pattern)]
    if not tests:
        raise SystemExit(f"No tests match pattern: {pattern}")

project_root = os.path.dirname(os.path.abspath(__file__))
test_dir = os.path.join(project_root, 'tests')

if args.recompile:
    removed_cache_count = clear_bytecode_cache(project_root)
    print(f"Cleared {removed_cache_count} bytecode cache file(s).")

roxalpath = 'build'
roxal = './roxal'

build_dir = os.path.join(project_root, roxalpath)

if args.build:
    jobs = os.cpu_count() or 4
    build_cmd = ['cmake', '--build', build_dir, f'-j{jobs}']
    print(f"Building Roxal ({' '.join(build_cmd)})...")
    try:
        subprocess.check_call(build_cmd)
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f"cmake build failed with exit code {exc.returncode}")

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
unexpected_failures = []

cwd = os.getcwd()
os.chdir(os.path.join(project_root, roxalpath))

features = detect_features(roxal)
has_grpc = 'grpc' in features
has_fileio = 'fileio' in features
has_dds = 'dds' in features
has_regex = 'regex' in features
has_xml = 'xml' in features
has_socket = 'socket' in features
has_nn = 'nn' in features
has_compute_server = 'server' in features
if not has_grpc and any(test in tests for test in grpc_tests):
    print("Skipping gRPC tests (feature not enabled).")
    tests = [t for t in tests if t not in grpc_tests]
if not has_fileio and any(test in tests for test in fileio_tests):
    print("Skipping fileio tests (feature not enabled).")
    tests = [t for t in tests if t not in fileio_tests]
if not has_dds:
    if any(test in tests for test in dds_tests):
        print("Skipping DDS tests (feature not enabled).")
        tests = [t for t in tests if t not in dds_tests]
if not has_regex:
    if any(test in tests for test in regex_tests):
        print("Skipping regex tests (feature not enabled).")
        tests = [t for t in tests if t not in regex_tests]
if not has_xml:
    if any(test in tests for test in xml_tests):
        print("Skipping XML tests (feature not enabled).")
        tests = [t for t in tests if t not in xml_tests]
if not has_socket:
    if any(test in tests for test in socket_tests):
        print("Skipping socket tests (feature not enabled).")
        tests = [t for t in tests if t not in socket_tests]
if not has_compute_server:
    if any(test in tests for test in compute_server_tests):
        print("Skipping compute server tests (feature not enabled).")
        tests = [t for t in tests if t not in compute_server_tests]
if not has_nn:
    if any(test in tests for test in nn_tests + nn_lfs_tests):
        print("Skipping ai.nn tests (feature not enabled).")
        tests = [t for t in tests if t not in nn_tests and t not in nn_lfs_tests]
has_media = 'media' in features
if not has_media:
    if any(test in tests for test in media_tests):
        print("Skipping media tests (feature not enabled).")
        tests = [t for t in tests if t not in media_tests]
if has_nn and any(test in tests for test in nn_lfs_tests):
    # Check that all LFS-tracked model files are available (not pointers or missing).
    # This covers any .onnx files tracked via .gitattributes LFS patterns.
    lfs_model_dir = os.path.join(project_root, 'modules', 'ai')
    lfs_models_available = True
    for f in os.listdir(lfs_model_dir):
        if f.endswith('.onnx') and is_lfs_pointer(os.path.join(lfs_model_dir, f)):
            lfs_models_available = False
            break
    if not lfs_models_available:
        print("Skipping LFS-dependent nn tests (model files not available; run 'git lfs pull').")
        tests = [t for t in tests if t not in nn_lfs_tests]
needs_grpc_server = has_grpc and any(test in tests for test in grpc_server_tests)
needs_compute_server = has_compute_server and any(test in tests for test in compute_server_tests)

env_base = os.environ.copy()
env_base['ROXALPATH'] = test_dir

def start_grpc_test_server(env) -> subprocess.Popen:
    script_path = os.path.join(project_root, 'scripts', 'grpc_everything_server.py')
    if not os.path.exists(script_path):
        raise RuntimeError(f"gRPC test server script not found at {script_path}")

    host, port_str = GRPC_TEST_ADDR.split(':', 1)
    port = int(port_str)
    proc = subprocess.Popen(
        [sys.executable, script_path, "--address", GRPC_TEST_ADDR],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )

    deadline = time.time() + 5.0
    last_error = None
    while time.time() < deadline:
        if proc.poll() is not None:
            output, _ = proc.communicate(timeout=0.1)
            raise RuntimeError(
                f"gRPC test server failed to start (exit {proc.returncode}): "
                f"{output.decode(errors='ignore')}"
            )
        try:
            with socket.create_connection((host, port), timeout=0.25):
                return proc
        except OSError as exc:
            last_error = exc
            time.sleep(0.1)

    proc.terminate()
    try:
        proc.wait(timeout=1)
    except subprocess.TimeoutExpired:
        proc.kill()
    raise RuntimeError(f"Timed out waiting for gRPC test server to start: {last_error}")


def start_compute_test_server(env, address: str) -> tuple[subprocess.Popen, str, tempfile.NamedTemporaryFile]:
    host, port_str = address.split(':', 1)
    port = int(port_str)
    log_handle = tempfile.NamedTemporaryFile(
        mode='w+b', suffix='.compute.log', prefix='roxal_compute_', delete=False
    )
    proc = subprocess.Popen(
        [roxal, '--server', '--port', str(port)],
        stdout=log_handle,
        stderr=subprocess.STDOUT,
        env=env,
    )

    deadline = time.time() + 5.0
    last_error = None
    while time.time() < deadline:
        if proc.poll() is not None:
            log_handle.flush()
            with open(log_handle.name, 'rb') as handle:
                output = handle.read()
            raise RuntimeError(
                f"compute server failed to start (exit {proc.returncode}): "
                f"{output.decode(errors='ignore')}"
            )
        try:
            with socket.create_connection((host, port), timeout=0.25):
                return proc, address, log_handle
        except OSError as exc:
            last_error = exc
            time.sleep(0.1)

    proc.terminate()
    try:
        proc.wait(timeout=1)
    except subprocess.TimeoutExpired:
        proc.kill()
    raise RuntimeError(f"Timed out waiting for compute server to start: {last_error}")


def read_new_server_output(log_handle: tempfile.NamedTemporaryFile, offset: int) -> tuple[int, bytes]:
    log_handle.flush()
    with open(log_handle.name, 'rb') as handle:
        handle.seek(offset)
        data = handle.read()
        new_offset = handle.tell()
    return new_offset, data


def run_compute_version_mismatch_test(address: str) -> tuple[bool, str]:
    host, port_str = address.split(':', 1)
    port = int(port_str)
    current_version = read_compute_protocol_version(project_root)
    wrong_version = 0 if current_version != 0 else 1
    payload = b'RXCS' + wrong_version.to_bytes(4, byteorder='big')
    frame = len(payload).to_bytes(4, byteorder='big') + bytes([0x01]) + payload

    with socket.create_connection((host, port), timeout=1.0) as sock:
        sock.sendall(frame)
        header = sock.recv(5)
        if len(header) != 5:
            return False, f"short response header: {header!r}"

        payload_len = int.from_bytes(header[:4], byteorder='big')
        msg_type = header[4]
        body = b''
        while len(body) < payload_len:
            chunk = sock.recv(payload_len - len(body))
            if not chunk:
                break
            body += chunk

    if msg_type != 0x03:
        return False, f"expected HELLO_ERR (0x03), got 0x{msg_type:02x}"
    if len(body) < 4:
        return False, f"HELLO_ERR payload too short: {body!r}"

    reason_len = int.from_bytes(body[:4], byteorder='big')
    reason = body[4:4 + reason_len].decode(errors='ignore')
    if "version mismatch" not in reason:
        return False, f"unexpected HELLO_ERR message: {reason!r}"
    return True, reason


def run_compute_refresh_here_test(test_dir: str, address: str, server_log_handle: tempfile.NamedTemporaryFile) -> tuple[bool, str]:
    temp_path = os.path.join(test_dir, 'remote_actor_refresh_here_runtime.rox')
    host_expr = address

    source_before = f"""type RefreshPrintActor actor:
  func run() -> int:
    print("client-copy")
    return 1

var worker = RefreshPrintActor() at "{host_expr}"
var result = worker.run()
wait(for=result)
print(result)
"""

    source_after = f"""type RefreshPrintActor actor:
  func run() -> int:
    print("server-copy", flush=true, here=true)
    return 2

var worker = RefreshPrintActor() at "{host_expr}"
var result = worker.run()
wait(for=result)
print(result)
"""

    try:
        with open(temp_path, 'w', encoding='utf-8') as handle:
            handle.write(source_before)
        first = subprocess.run([roxal, temp_path], capture_output=True, env=env_base, timeout=TEST_TIMEOUT_SECS)
        if first.returncode != 0:
            return False, f"first run failed: {first.stderr.decode(errors='ignore')}"
        if first.stdout != b'client-copy\n1\n':
            return False, f"unexpected first stdout: {first.stdout!r}"

        server_offset = os.path.getsize(server_log_handle.name)
        with open(temp_path, 'w', encoding='utf-8') as handle:
            handle.write(source_after)
        second = subprocess.run([roxal, '--recompile', temp_path], capture_output=True, env=env_base, timeout=TEST_TIMEOUT_SECS)
        if second.returncode != 0:
            return False, f"second run failed: {second.stderr.decode(errors='ignore')}"
        if second.stdout != b'2\n':
            return False, f"unexpected second stdout: {second.stdout!r}"

        _, server_chunk = read_new_server_output(server_log_handle, server_offset)
        if server_chunk != b'server-copy\n':
            return False, f"unexpected server stdout after refresh: {server_chunk!r}"
        return True, "refresh here=true respected without server restart"
    finally:
        try:
            os.remove(temp_path)
        except FileNotFoundError:
            pass

grpc_server_proc = None
compute_server_proc = None
compute_server_proc_2 = None
compute_server_log = None
compute_server_log_2 = None
compute_test_addr = None
compute_test_addr_2 = None
generated_compute_tests = []
compute_server_output = ""
total_start_time = time.perf_counter()

try:
    grpc_server_proc = start_grpc_test_server(env_base) if needs_grpc_server else None
    if needs_compute_server:
        compute_server_proc, compute_test_addr, compute_server_log = start_compute_test_server(env_base, COMPUTE_TEST_ADDR)
        if any(test in tests for test in compute_server_double_hop_tests):
            compute_server_proc_2, compute_test_addr_2, compute_server_log_2 = start_compute_test_server(env_base, COMPUTE_TEST_ADDR_2)

    for test in tests:
        print(f"Test {test:<{TEST_NAME_WIDTH}} ", end='', flush=True)
        start_time = time.perf_counter()
        if test == 'remote_actor_version_mismatch':
            passed, detail = run_compute_version_mismatch_test(compute_test_addr)
            duration_ms = (time.perf_counter() - start_time) * 1000
            if passed:
                print(f"pass ({duration_ms:.0f} ms)", flush=True)
                passed_count += 1
            else:
                print("FAIL:", flush=True)
                print(detail)
                failed_count += 1
                unexpected_failures.append(test)
            continue
        if test == 'remote_actor_refresh_here':
            passed, detail = run_compute_refresh_here_test(test_dir, compute_test_addr, compute_server_log)
            duration_ms = (time.perf_counter() - start_time) * 1000
            if passed:
                print(f"pass ({duration_ms:.0f} ms)", flush=True)
                passed_count += 1
            else:
                print("FAIL:", flush=True)
                print(detail)
                failed_count += 1
                unexpected_failures.append(test)
            continue

        testrox = os.path.join(test_dir, test + '.rox')
        testout = os.path.join(test_dir, test + '.out')
        testerr = os.path.join(test_dir, test + '.err')
        if not os.path.exists(testrox):
            raise RuntimeError(f"Test {testrox} not found.")

        if not (os.path.exists(testout) or os.path.exists(testerr)):
            raise RuntimeError(f"Test expected output {testout} or {testerr} not found.")

        run_testrox = testrox
        compute_server_offsets = {}
        if test in compute_server_tests:
            with open(testrox, 'r', encoding='utf-8') as handle:
                source = handle.read()
                source = source.replace(COMPUTE_TEST_ADDR_PLACEHOLDER, compute_test_addr)
                source = source.replace(COMPUTE_TEST_ADDR_2_PLACEHOLDER,
                                        compute_test_addr_2 if compute_test_addr_2 else COMPUTE_TEST_ADDR_2)
            temp_handle = tempfile.NamedTemporaryFile(
                mode='w', suffix='.rox', prefix=f'{test}_', dir=test_dir, delete=False, encoding='utf-8'
            )
            with temp_handle:
                temp_handle.write(source)
            run_testrox = temp_handle.name
            generated_compute_tests.append(run_testrox)
            if compute_server_log:
                compute_server_offsets[compute_server_log.name] = os.path.getsize(compute_server_log.name)
            if compute_server_log_2:
                compute_server_offsets[compute_server_log_2.name] = os.path.getsize(compute_server_log_2.name)

        rel_testrox = os.path.relpath(run_testrox, os.getcwd())
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
        if test.startswith('grpc_'):
            proto_path = os.path.join('..', 'compiler', 'grpc', 'protos')
            cmd = [cmd[0], '-p', proto_path, *cmd[1:]]

        if args.opcode_prof and '--opcode-prof' not in cmd:
            cmd = [cmd[0], '--opcode-prof', *cmd[1:]]
        if args.nocache and '--nocache' not in cmd:
            cmd = [cmd[0], '--nocache', *cmd[1:]]
        if args.nogc and '--nogc' not in cmd:
            cmd = [cmd[0], '--nogc', *cmd[1:]]

        opt_expected = (" [expected]" if test in failing_tests else '')

        if test == 'gc_stress':
            timeout_secs = GC_STRESS_TIMEOUT_SECS
        elif test in nn_lfs_tests:
            timeout_secs = NN_LFS_TIMEOUT_SECS
        else:
            timeout_secs = TEST_TIMEOUT_SECS

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
            if test not in failing_tests:
                unexpected_failures.append(test)
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
        server_out_path = os.path.join(test_dir, test + '.server.out')
        if os.path.exists(server_out_path):
            expected_server = open(server_out_path, 'rb').read()
            actual_chunks = []
            if compute_server_log and compute_server_log.name in compute_server_offsets:
                _, chunk = read_new_server_output(compute_server_log, compute_server_offsets[compute_server_log.name])
                actual_chunks.append(chunk)
            if compute_server_log_2 and compute_server_log_2.name in compute_server_offsets:
                _, chunk = read_new_server_output(compute_server_log_2, compute_server_offsets[compute_server_log_2.name])
                actual_chunks.append(chunk)
            actual_server = b''.join(actual_chunks)
            if expected_server != actual_server:
                print(f"FAIL: {opt_expected}", flush=True)
                print("-- compute server stdout --")
                print(actual_server)
                print("-- expected compute server stdout --")
                print(expected_server)
                print("--")
                print()
                passed = False
        if not passed and compProc.stderr:
            print("-- stderr --")
            print(compProc.stderr.decode())
            print("--")
            print()
        if passed:
            print(f"pass ({duration_ms:.0f} ms)", flush=True)
            passed_count += 1
        else:
            print(f"({duration_ms:.1f} ms)", flush=True)
            failed_count += 1
            if test not in failing_tests:
                unexpected_failures.append(test)

except Exception as e:
    print('Exception: ' + str(e))
finally:
    for path in generated_compute_tests:
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
    if compute_server_proc:
        compute_server_proc.terminate()
        try:
            compute_server_proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            compute_server_proc.kill()
        if compute_server_log:
            try:
                compute_server_log.flush()
                with open(compute_server_log.name, 'rb') as handle:
                    compute_server_output = handle.read().decode(errors='ignore')
            except Exception:
                compute_server_output = ""
            try:
                compute_server_log.close()
            except Exception:
                pass
            try:
                os.remove(compute_server_log.name)
            except OSError:
                pass
    if compute_server_proc_2:
        compute_server_proc_2.terminate()
        try:
            compute_server_proc_2.wait(timeout=2)
        except subprocess.TimeoutExpired:
            compute_server_proc_2.kill()
        if compute_server_log_2:
            try:
                compute_server_log_2.flush()
                with open(compute_server_log_2.name, 'rb') as handle:
                    extra_output = handle.read().decode(errors='ignore')
                compute_server_output = (compute_server_output + "\n" + extra_output).strip()
            except Exception:
                pass
            try:
                compute_server_log_2.close()
            except Exception:
                pass
            try:
                os.remove(compute_server_log_2.name)
            except OSError:
                pass
    if grpc_server_proc:
        grpc_server_proc.terminate()
        try:
            grpc_server_proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            grpc_server_proc.kill()
    os.chdir(cwd)

total_duration = time.perf_counter() - total_start_time
failed_unexpected_count = len(unexpected_failures)
expected_fail_count = len([t for t in failing_tests if t in tests])
expected_fail_msg = f" ({expected_fail_count} were expected to fail)" if expected_fail_count > 0 else ""
print()
print(f"{passed_count} tests passed, {failed_unexpected_count} "+('FAILED' if failed_unexpected_count>0 else 'failed')+f" unexpectedly{expected_fail_msg}")
if unexpected_failures:
    print(f"Unexpected failures: {', '.join(unexpected_failures)}")
if compute_server_output and any(test in compute_server_tests for test in unexpected_failures):
    print("-- compute server output --")
    print(compute_server_output)
    print("--")
print(f"Total time {total_duration:.2f} s")

if args.opcode_prof:
    if os.path.exists(opcode_profile_path):
        print(f"Opcode profile written to {opcode_profile_path}")
    else:
        print(f"Opcode profiling was requested but {opcode_profile_path} was not created.")
