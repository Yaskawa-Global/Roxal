#!/usr/bin/env python3

from __future__ import annotations
import os
import sys
import socket
import subprocess
import argparse
import fnmatch
import glob
import re
import time
import tempfile
from typing import Set

# Maximum time in seconds to allow each test to run
TEST_TIMEOUT_SECS = 5
GC_STRESS_TIMEOUT_SECS = 20
NN_LFS_TIMEOUT_SECS = 60
DOOM_TIMEOUT_SECS = 60
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


def direct_needed_libs(binary: str):
    """Return the ELF DT_NEEDED (direct) shared-library names of `binary`, or None if no
    reader tool (objdump/readelf) is available."""
    for cmd in (['objdump', '-p', binary], ['readelf', '-d', binary]):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, check=False).stdout
        except FileNotFoundError:
            continue
        needed = []
        for line in out.splitlines():
            if 'NEEDED' not in line:
                continue
            if '[' in line and ']' in line:        # readelf:  ... Shared library: [libfoo.so.1]
                needed.append(line[line.index('[') + 1:line.index(']')])
            else:                                   # objdump:  NEEDED   libfoo.so.1
                needed.append(line.split()[-1])
        return needed
    return None


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
    'nil_to_ref_types', 'nil_to_value_type_err', 'nil_to_range_err', 'nil_to_typed_prop_err', 'range_content_eq',
    'arith', 'factorial', 'defaultvalues', 'construct_defaults', 'typeof_test', 'invoke_method',
    'change_notifier', 'gc_nested_invoke', 'gc_construct_stress', 'gc_coordination_stress', 'gc_selftest', 'gc_scanner_selftest',
    'dict', 'dict2', 'dict_keyerror', 'dict_dot', 'dict_dot_keyerror', 'dict_self_reference', 'list', 'list2', 'list_negative_index', 'list_self_reference', 'copyinto_list', 'copyinto_list_unicode', 'copyinto_sublist', 'copyinto_signal',
    'list_add_test', 'list_concat_shallow', 'list_methods', 'list_add_nonlist_err', 'list_remove_notfound_err', 'list_pop_empty_err', 'list_dict_equal', 'test_filter_map_reduce', 'list_method_exception', 'test_paren_continuation',
    'list_packed_repr', 'list_packed_semantics', 'list_packed_transitions', 'list_packed_reserve', 'list_packed_const', 'list_packed_serialize', 'range', 'range2', 'enum1', 'enum2', 'enum3', 'upvalue_leak',
    'unicode', 'backtick_identifier', 'literal_bases', 'literal_base_clash_err', 'signal_clock', 'signal_add', 'signal_subtract', 'signal_multiply', 'signal_divide', 'signal_modulo',
    'signal_greater', 'signal_less', 'signal_equal', 'signal_history', 'signal_cycle', 'signal_cleanup',
    'signal_and', 'signal_or', 'signal_not', 'signal_band', 'signal_bor', 'signal_bxor', 'signal_bnot',
    'signal_func_nocall', 'signal_func_exec', 'signal_index', 'signal_when_stmt', 'signal_when_threads', 'when_expression', 'signal_when_in_method', 'signal_when_becomes', 'signal_on_changed_test',
    'module_var_when_changed', 'module_var_when_changed_string', 'module_var_when_becomes', 'object_member_when_changed', 'when_obj_becomes', 'when_accessor_var_changes',
    'test_signal_value_property', 'test_signal_name_property', 'signal_named_param', 'construct_by_signal', 'signal_run_stop', 'signal_source', 'signal_default_err', 'signal_network1',
    'signal_islands', 'signal_domain', 'signal_tensor_isolation', 'signal_tensor_const',
    'multi_return', 'multi_return_arity_err', 'multi_return_nonlist_err', 'multi_return_literal_err', 'test_multi_return_syntax',
    'signal_multi_output', 'signal_wiring_func', 'signal_wiring_mixed_err', 'signal_branch_err', 'signal_sampling',
    'signal_list_const', 'df_const_arg_err', 'inspect_df_structure', 'signal_lift_fresh', 'signal_lift_nodisturb',
    'signal_copyinto_freq_err',
    'var_destructure', 'var_destructure_arity_err', 'var_destructure_nonlist_err', 'var_destructure_const_err',
    'signal_shift', 'signal_deduce', 'signal_variadic_err', 'check_compile_err', 'signal_sampled', 'signal_feedback_rate', 'signal_island_rates', 'bitwise_large_int', 'signal_nolift_wait', 'inline_lambda_assign_err',
    'dataflow_clocktest1', 'multi_clock', 'clock_error', 'clock_name_param',
    'event1', 'event_when_stmt', 'event_emit_keyword', 'event_when_method', 'event_remove_method', 'event_ref', 'event_actor_ref', 'event_actor_ref2', 'event_actor_ref3', 'event_actor_ref4', 'event_instance_emit',
    'event_payload', 'event_implicit_constructor', 'event_type_when', 'event_target_filter',
    'event_in_sleep', 'event_in_sleep2', 'event_cascade', 'event_chain_depth',
    'until_event', 'until_signal', 'signal_vector_dot',
    'if_suffix_basic', 'if_suffix_assignment', 'if_suffix_mutex_err',
    'nonstrict-assign', 'nonstrict-assign-err', 'strict-assign', 'strict-assign-err',
    'module_strict_assign_err', 'var_redeclare_err', 'var_redeclare_assign_err', 'repl_var_redeclare_err', 'func_nonstrict', 'conversions1',
    'serialize_values', 'serialize_signal', 'serialize_objects', 'serialize_user_objects', 'serialize_func', 'serialize_actor',
    'json_basic',
    'json_dict_order',
    'json5_comments', 'json5_trailing_comma', 'json5_unquoted_keys',
    'json5_single_quotes', 'json5_numbers', 'json5_string_escapes',
    'json5_writer', 'json5_nonfinite', 'json5_nonfinite_err',
    'json5_roundtrip',
    'byteops', 'bitwise', 'byte_int_bits', 'int64_promotion', 'int64_bounds', 'list_byte_concat', 'list_enum_concat',
    'object_init', 'object_constructor_args', 'object_constructor_unknown_arg', 'object_constructor_arg_count',
    'object_inherit_is', 'object_downcast', 'object_ref_member_default',
    'star_init_basic', 'star_init_body_post_action', 'star_init_coexist',
    'star_init_inherited', 'star_init_empty',
    'star_init_err_outside_init', 'star_init_err_mixed_params', 'star_init_err_actor',
    'star_init_err_duplicate_sig', 'star_init_err_const_member',
    'star_init_accessor_setter_bypass', 'star_init_accessor_getonly',
    'star_init_accessor_with_data', 'star_init_accessor_default',
    'star_init_accessor_err_getonly_unreachable',
    'star_init_no_initializer', 'star_init_body_len_iter',
    'closure', 'closure2', 'closure3', 'closure4', 'closure5', 'closure_many', 'lambda1', 'lambda2',
    'conversion1', 'string_interp', 'string_case',
    # Deliberately NOT in regex_tests: split/search on literal text must behave
    # identically with the regex engine on and off.
    'string_split_plain',
    'string_interp_escape', 'string_interp_triple', 'string_interp_suffix', 'string_interp_many',
    'string_interp_err_unterm', 'string_interp_err_empty', 'string_interp_err_colon',
    'string_interp_err_comment', 'string_interp_err_junk', 'string_interp_err_assign',
    'string_interp_err_docstring', 'string_interp_err_annot', 'typededucer_string_interp',
    'call_param_type_nonstrict', 'call_param_type_strict', 'param_assign_static_err',
    'linkedlist', 'structbindassign',
    'if', 'for1', 'nested_for',
    'break_simple', 'continue_simple', 'break_while', 'continue_for_advance',
    'break_nested', 'break_with_locals', 'break_in_match',
    'break_outside_loop', 'continue_outside_loop',
    'builtin_stub_func', 'builtin_stub_method',  # unlinked @builtin raises, never silently no-ops
    'jump_backward_retry', 'jump_nested_loop_exit', 'jump_forward_out_of_if',
    'jump_if_clause', 'jump_locals_cleanup', 'typededucer_jump_label',
    'jump_undefined', 'jump_into_block', 'jump_skips_var', 'jump_cross_try',
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
    'import_clash', 'import_asset_sibling',
    'cachereload_builtin',
    'method_named_param',
    'annot1', 'annot_import', 'annot_file_level', 'annot_ros_nonidl', 'generic', 'objscopes',
    'threads1', 'fork_upvalue_error', 'fork_no_upvalues',
    'actor1', 'actor2', 'actor3', 'actor4', 'actor5', 'actor6', 'actor7', 'actor8', 'actor9',
    'actor_init', 'actor_stack', 'actor_future', 'future_ready', 'future_builtin_resolve', 'future_typed_param_resolve', 'wait_duration', 'wait_duration_dim_err', 'wait_duration_mixed_err',
    'allof_futures', 'anyof_futures', 'anyof_event', 'anyof_signal',
    'allof_empty', 'anyof_empty', 'allof_list_arg', 'nested_combinators',
    'anyof_cleanup', 'anyof_exception',
    'actor_method_order',
    'actor_closure1', 'actor_closure2', 'actor_closure3',
    'actor_inter',
    'clone1', 'clone_shared', 'clone_cycle', 'extends1', 'nothis', 'superprop', 'scopetest4', 'local_type_scope',
    'const_member_type_access', 'const_member_type_var_err', 'const_member_type_mutable_err', 'const_member_type_private_err',
    'const_member_freeze', 'const_member_shared', 'const_member_untyped_freeze',
    'nested_type_enum', 'nested_type_object', 'nested_type_inherit',
    'nested_type_extends', 'nested_type_implements',
    'nested_type_implements_deep',
    'nested_type_sibling', 'nested_type_private', 'nested_type_shadow',
    'nested_type_sibling_property', 'property_accessor_then_nested_type',
    'dotted_type_name', 'dotted_type_implements', 'dotted_type_deep', 'dotted_type_enum_anno', 'dotted_type_err',
    'interface_basic', 'interface_var_satisfies_accessors', 'interface_const_satisfies_get',
    'interface_extends', 'interface_layered', 'interface_multi', 'interface_setter_only_iface',
    'interface_nested',
    'interface_sugar_var',
    'interface_const_no_init', 'interface_const_accessor_no_init',
    'interface_concrete_const', 'interface_concrete_const_override',
    'interface_concrete_inherited_through_extends',
    'interface_missing_method', 'interface_missing_setter', 'interface_missing_getter',
    'interface_const_cannot_satisfy_set',
    'interface_concrete_accessor_in_iface', 'interface_abstract_accessor_outside_iface',
    'interface_extends_object',
    'interface_no_instantiate', 'interface_implements_interface',
    'interface_var_with_init',
    'is_subtype',
    'private_prop', 'private_method', 'private_inherit',
    'operator_overload', 'operator_overload_cmp', 'operator_overload_commutative',
    'operator_overload_lr', 'operator_overload_unary_lr', 'operator_overload_inherit', 'operator_overload_fallthrough',
    'operator_overload_proc_err', 'operator_overload_unpaired_err', 'operator_overload_both_err',
    'overload_basic', 'overload_runtime', 'overload_default_param', 'overload_variadic',
    'overload_untyped', 'overload_local', 'overload_subtype', 'overload_ambiguous',
    'overload_cross_module', 'overload_implicit_conv', 'overload_user_conv',
    'overload_method_basic', 'overload_method_init', 'overload_method_inheritance',
    'overload_method_bound', 'overload_method_actor', 'overload_method_ambiguous',
    'overload_interface_basic', 'overload_interface_partial',
    'overload_interface_covariant_return',
    'overload_named_args', 'overload_default_named', 'overload_dotted_module',
    'overload_no_implicit_init', 'overload_method_chain',
    'overload_first_class',
    'complex_type',
    'typededucer_binop', 'typededucer_ops', 'typededucer_until', 'typededucer_if_suffix', 'typededucer_bitwise',
    'time_basic', 'time_quantity', 'time_quantity_arith',
    'cont_nest_print', 'cont_nest_map', 'cont_nest_filter',
    'cont_nest_map_in_map', 'cont_nest_filter_in_map', 'cont_nest_reduce_in_map',
    'cont_nest_print_in_opstr', 'cont_nest_closure_conv',
    'mathfuncs',
    'typeassign1', 'typeassign2', 'typeassign3',
    'vector1', 'vector2', 'vector3', 'vector4', 'vector5','vector_methods', 'vector_equal', 'vector_matrix_equal',
    'vector_list_disambig', 'vector_list_ambig_err', 'vector_paren_elem',
    'matrix1', 'matrix2', 'matrix_literal1', 'matrix_literal_newline', 'vector_matrix_negative', 'unary_vector_matrix',
    'matrix_index', 'matrix_methods', 'matrix_assign', 'matrix_equal', 'matrix_math',
    'vector_quantity_test', 'orient_test', 'orient_conv_test',
    'tensor_basic', 'tensor_math', 'tensor_compare', 'tensor_convert', 'math_min_max_sum',
    'tensor_convert_err', 'matrix_tensor_err', 'vector_tensor_err',
    'tensor_slice', 'tensor_slice_assign', 'tensor_slice_assign_err', 'tensor_slice_assign_type_err',
    'tensor_inplace', 'tensor_inplace_divzero_err', 'tensor_blit',
    'tensor_bytes', 'tensor_bytes_move', 'tensor_dtype_storage',
    'tensor_take', 'tensor_take_err',
    'tensor_bytes_len_err', 'tensor_bytes_conflict_err', 'tensor_uint16',
    'tensor_introspect',
    'math_relu', 'math_softmax', 'math_argmax', 'math_clamp', 'math_abs',
    'value_semantics', 'value_semantics_cow',
    'weakref', 'strongref', 'is_operator', 'in_operator', 'stackdepth', 'modulevar2',
    'const_basic', 'const_assign_err', 'const_nonliteral_err', 'const_missing_initializer_err',
    'const_property', 'const_property_method_err', 'const_property_runtime_err', 'const_module_assign',
    'const-interior-mutation',
    'const_list', 'const_dict', 'const_nested', 'const_snapshots', 'const_alias', 'const_identity',
    'const_deep_chain', 'const_cycle', 'const_diamond', 'const_multi_snapshot', 'const_func', 'const_escape_err',
    'const_type_qualifier', 'const_tensor_freeze', 'const_mutable_type', 'const_builtin_method_err', 'const_linked_method_err', 'const_mvcc',
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
    'except_type_mismatch', 'except_type_mismatch_err',
    'zero_division', 'zero_division_uncaught_err', 'zero_division_actor',
    'stacktrace', 'exception_stacktrace', 'object_user_ref_cycle', 'gc_list_cycle', 'gc_liveness',
    'property_count', 'property_accessor', 'property_accessor_oneliner', 'dict_property_getters', 'cmdline_execute', 'repl_run', 'invalid_option', 'fileio_basic', 'fileio_binary',
    'fileio_read_binary', 'fileio_write_binary', 'fileio_actor_write', 'fileio_delete', 'fileio_extra', 'fileio_packed',
    'fileio_sync', 'fileio_async_param', 'fileio_list_dir',
    'string_concat_roundtrip', 'actor_concat_stress',
    'help_doc', 'help_wait', 'help_time_wall_now', 'help_time_wall_now_instance', 'docstring_func',
    'builtin_object_methods', 'math_counter_signal', 'print_flush', 'sys_paths',
    'sys_platform', 'sys_defined',
    'grpc_message_types', 'grpc_service_actor', 'grpc_int64_values', 'grpc_runtime_error', 'grpc_streaming', 'grpc_args',
    'rt_execution',
    'operator_conv_string', 'operator_conv_string_rettype', 'return_type_conv', 'return_type_conv_upcast',
    'operator_conv_string_inherit',
    'operator_conv_string_implicit',
    'operator_conv_proc_err', 'operator_conv_arity_err', 'operator_conv_rettype_err',
    'operator_conv_object',
    'suffix_basic', 'suffix_braced', 'suffix_compound', 'suffix_string',
    'suffix_unknown_err', 'suffix_edge_cases',
    'suffix_pct', 'suffix_pct_modulo_err', 'suffix_pct_register_err',
    'sci_notation', 'sci_notation_suffix_err',
    'suffix_angular_accel_jerk', 'suffix_linear_jerk',
    'quantity_basic', 'quantity_from_string',
    'quantity_dim_predicates', 'quantity_movj_pattern',
    'vector_mixed_dim', 'vector_mixed_bare_err',
    'call_too_many_args_err',
    'bits_bytes',
    'conv_explicit_default',
    'conv_constructor_auto', 'conv_constructor_explicit',
    'conv_func_param_auto',
    'stmt_action_basic', 'stmt_action_chain', 'stmt_action_until',
    'stmt_action_ignore', 'stmt_action_cycle_err',
    'if_suffix_stmt_action',
    'stack_depth_check', 'dispatch_rare_interleave',
    'forward_decl_field', 'forward_decl_chain', 'forward_decl_module_var'
]

grpc_tests = ['grpc_message_types', 'grpc_service_actor', 'grpc_int64_values', 'grpc_runtime_error', 'grpc_streaming', 'grpc_args']
grpc_server_tests = ['grpc_int64_values', 'grpc_streaming', 'grpc_args']
fileio_tests = [
    'fileio_basic', 'fileio_binary', 'fileio_read_binary', 'fileio_write_binary',
    'fileio_actor_write', 'fileio_delete', 'fileio_extra', 'fileio_packed',
    'fileio_sync', 'fileio_async_param', 'fileio_list_dir',
    'string_concat_roundtrip', 'actor_concat_stress'
]
dds_tests = ['dds_bounded_ok', 'dds_bounded_fail', 'dds_complex_smoke', 'dds_array_ok', 'dds_array_struct', 'dds_array_multi', 'dds_nested_module',
             'dds_idl_include', 'dds_idl_include_missing', 'dds_idl_stock',
             'dds_ros_import', 'dds_ros_signal_roundtrip', 'dds_ros_camerainfo',
             'dds_signal_keepall', 'dds_signal_keeplast', 'dds_ros_signal_lift']
regex_tests = ['regex_test']
inspect_tests = [
    'inspect_parse', 'inspect_fields', 'inspect_walk', 'inspect_parent',
    'inspect_positions', 'inspect_comments', 'inspect_schema',
    'inspect_tolerant', 'inspect_parse_err',
    'inspect_network', 'inspect_network_values', 'inspect_signals_enum',
    'inspect_network_lifecycle', 'inspect_network_provenance',
    'inspect_unparse', 'inspect_edit', 'inspect_fragments',
    'inspect_fragment_err', 'inspect_unparse_err', 'inspect_roundtrip_corpus',
    'inspect_compile', 'inspect_compile_err', 'inspect_annot_roundtrip',
    # dfdoc (diagram document library) is pure Roxal over inspect
    'dfdoc_ops_basic', 'dfdoc_names', 'dfdoc_feedback', 'dfdoc_load_save',
    'dfdoc_comments', 'dfdoc_runs', 'dfdoc_palette', 'dfdoc_check',
    'dfdoc_input_types', 'dfdoc_typecheck', 'dfdoc_live', 'dfdoc_compose', 'dfdoc_compose_run',
]
xml_tests = [
    'xml_basic_compact', 'xml_basic_raw', 'xml_attrs', 'xml_mixed_raw',
    'xml_compact_lossy', 'xml_whitespace', 'xml_to_xml_compact',
    'xml_to_xml_raw', 'xml_invalid', 'xml_mode_errors',
    'xml_write_mode_error', 'xml_shape_error'
]
socket_tests = ['socket_basic']
# @cfunc / @cstruct / sys.loadlib against tests/testlib.so; need ROXAL_ENABLE_FFI
ffi_tests = [
    'ffi1', 'ffi_addfloats', 'ffi_struct_out', 'ffi_inttypes', 'ffi_strlen', 'ffi_relative',
    'ffi_toupper', 'ffi_primptr', 'ffi_voidptr_struct', 'cstruct1', 'cstruct2', 'cstruct3',
    'cstruct_byval', 'cstruct_array',
    'ffi_int64', 'ffi_ptr_return', 'ffi_free', 'ffi_tensor', 'ffi_tensor_mismatch_err',
    'ffi_nullptr', 'ffi_blocking',
    'ffi_ptrptr_slot', 'ffi_ptrptr_nil_err',
    'nested_cstruct', 'nested_cstruct_ptr', 'nested_cstruct_byval', 'nested_cstruct_align',
    'nested_cstruct_infer', 'cstruct_array_struct', 'cstruct_array_overflow_err',
]
nn_tests = ['nn_mnist', 'nn_signal', 'nn_chain', 'nn_signal_chain', 'nn_dynamic', 'nn_multi_io', 'nn_async', 'nn_tokenizer']
nn_lfs_tests = ['nn_dfine']  # require LFS model files (only run with --all)
media_tests = ['media_read_write', 'media_manipulate', 'media_convert',
               # audio: run with ROXAL_AUDIO_BACKEND=null (no hardware needed)
               'media_audio_basic', 'media_audio_play', 'media_audio_record',
               'media_audio_err_none', 'media_audio_err_rate',
               'media_audio_err_dtype', 'media_audio_err_format']
# pure-Roxal FFI binding over the cvx shim; needs modules/opencv/libcvxshim.so built
opencv_tests = ['opencv_basic', 'opencv_imgproc', 'opencv_imgproc2', 'opencv_draw',
                'opencv_video', 'opencv_writer', 'opencv_imread_err',
                'opencv_codec', 'opencv_aruco', 'opencv_calib', 'opencv_handeye',
                'opencv_features', 'opencv_charuco', 'opencv_stereo_calib',
                'opencv_homography', 'opencv_blob',
                'opencv_segment', 'opencv_contour_kit', 'opencv_template_qr',
                'opencv_flow', 'opencv_reproject', 'opencv_fisheye',
                'opencv_depth']
# DNN task wrappers additionally need the downloaded models
# (modules/opencv/models/download-models.sh)
opencv_dnn_tests = ['opencv_face', 'opencv_aliked', 'opencv_track']
opencv_tests += opencv_dnn_tests
# pure-Roxal FFI binding over the rs shim; needs modules/realsense/librsshim.so
# built AND a RealSense camera plugged in (both gated below)
realsense_tests = ['realsense_basic']
qt_tests = ['qt_lifecycle', 'qt_load_file',           # P0: lifecycle
            'qt_properties', 'qt_property_error', 'qt_convert',  # P1: handles/props/methods/convert
            'qt_signal_callback', 'qt_signal_event', 'qt_signal_args',  # P2: signals -> callbacks/events
            'qt_disconnect', 'qt_signal_gc',
            'qt_model_basic', 'qt_model_qml', 'qt_model_edit',  # P3: list model
            'qt_model_struct', 'qt_model_gc',
            'qt_bind_read', 'qt_bind_write', 'qt_bind_auto', 'qt_bind_gc',  # P4: bindable object
            'qt_bind_computed',   # computed (accessor) properties as bound roles
            'qt_bind_method',     # QML calling bound-object methods (moc-free bridge)
            'qt_callback_error', 'qt_callback_catch',  # handler-exception behavior (fail-loud / catch)
            'qt_tree_basic', 'qt_tree_edit', 'qt_tree_gc', 'qt_tree_qml',  # tree model (QAbstractItemModel)
            'qt_sortfilter_basic', 'qt_sortfilter_qml',  # sort/filter proxy (QSortFilterProxyModel)
            'qt_table_basic', 'qt_table_qml', 'qt_table_gc', 'qt_table_badcol',  # table model (QAbstractTableModel)
            'qt_dynamic_create',  # runtime item instantiation (qt.Component)
            'qt_timer',           # cooperative pumping: qt.every / run_for / process_events
            'qt_callback_reentrancy',  # busy-pump must not re-enter a running main-thread callback
            'qt_style',           # qt.set_style (Qt Quick Controls style selection)
            'qt_actor_guard',     # Qt access from an actor thread is rejected
            'qt_log',             # Qt/QML message redirection
            'qt_load_relative',   # script-relative QML path resolution
            'qt_image_convert',   # uint8 [H,W,C] tensor <-> QImage round-trips
            'qt_image_convert_error',  # non-uint8 tensor -> image rejected
            'qt_frameview',       # FrameView: present/frame, software render, grab_window pixel-exact
            'qt_frameview_badshape',   # unsupported channel count rejected
            'qt_keys']            # Keys handlers -> Roxal via signal; qt.post_key
# require Qt6 (ROXAL_ENABLE_QT); run headless via QT_QPA_PLATFORM=offscreen
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
tests += inspect_tests
tests += xml_tests
tests += socket_tests
tests += ffi_tests
tests += nn_tests
tests += media_tests
tests += qt_tests
tests += compute_server_tests

long_running_tests = [
    'gc_stress',
    'const_mvcc_stress',
]

# doom example tests (examples/doom in-development port; only run with --all)
doom_tests = ['doom_wad', 'doom_gfx', 'doom_render', 'doom_game', 'doom_sound', 'doom_pickup']

# implementation doesn't yet allow these tests to pass (do not add to this list without human consent)
failing_tests = []
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
    tests += doom_tests
    tests += opencv_tests   # also require modules/opencv/libcvxshim.so (gated below)
    tests += realsense_tests  # also require librsshim.so + a camera (gated below)

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

roxalpath = os.environ.get('ROXAL_BUILD_DIR', 'build')  # env override for sanitizer/forensic builds
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

# NOTE: building tests/testlib.so happens after feature detection below — an
# FFI-less build has no use for it and must not require a working gcc.


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
has_ffi = 'ffi' in features
has_nn = 'nn' in features
has_compute_server = 'server' in features
has_qt = 'qt' in features
has_icu = 'icu' in features

# Distributable-property guard: when this build supports qt (via the dlopen'd plugin),
# the roxal binary itself must NOT directly depend on Qt — otherwise it won't start on
# machines without Qt, defeating the whole point of the plugin split. Fail loudly if Qt
# ever creeps back into the binary's NEEDED entries.
if has_qt:
    needed = direct_needed_libs(roxal)
    if needed is None:
        print("Note: skipping no-Qt-dependency check (no objdump/readelf available).")
    else:
        qt_needed = [n for n in needed if 'Qt' in n or n.startswith('libroxalqt')]
        if qt_needed:
            print(f"CHECK FAILED: roxal binary directly depends on Qt ({', '.join(qt_needed)}); "
                  "the qt module must remain a dlopen'd plugin (libroxalqt.so), not linked in.")
            unexpected_failures.append('no_qt_dependency')
        else:
            print("Check: roxal binary carries no direct Qt dependency (qt loads as a plugin). OK")

if has_ffi:
    # Ensure the FFI test shared library is built (only meaningful with FFI enabled)
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
            raise SystemExit('testlib.so was not built')
else:
    if any(test in tests for test in ffi_tests):
        print("Skipping FFI tests (feature not enabled).")
        tests = [t for t in tests if t not in ffi_tests]
    # opencv/realsense are pure-Roxal @cfunc bindings over a shim .so, so they
    # need FFI too — independently of whether their shim happens to be built.
    _ffi_modules = opencv_tests + realsense_tests
    if any(test in tests for test in _ffi_modules):
        print("Skipping opencv/realsense tests (FFI feature not enabled).")
        tests = [t for t in tests if t not in _ffi_modules]

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
has_inspect = 'inspect' in features
if not has_inspect:
    if any(test in tests for test in inspect_tests):
        print("Skipping inspect tests (feature not enabled).")
        tests = [t for t in tests if t not in inspect_tests]
if not has_socket:
    if any(test in tests for test in socket_tests):
        print("Skipping socket tests (feature not enabled).")
        tests = [t for t in tests if t not in socket_tests]
if not has_qt:
    if any(test in tests for test in qt_tests):
        print("Skipping qt tests (feature not enabled).")
        tests = [t for t in tests if t not in qt_tests]
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
if not has_icu:
    unicode_case_tests = ['string_case', 'string_interp_suffix']
    if any(test in tests for test in unicode_case_tests):
        print("Skipping Unicode case-mapping tests (ICU backend not enabled).")
        tests = [t for t in tests if t not in unicode_case_tests]
realsense_shim = os.path.join(project_root, 'modules', 'realsense', 'librsshim.so')


def realsense_connected() -> bool:
    """True if a RealSense is on the USB bus (network cameras are not detected;
    skipping the tests is the safe answer there)."""
    for product in glob.glob('/sys/bus/usb/devices/*/product'):
        try:
            with open(product) as f:
                if 'RealSense' in f.read():
                    return True
        except OSError:
            pass
    return False


if any(test in tests for test in realsense_tests):
    if not os.path.exists(realsense_shim):
        print("Skipping realsense tests (modules/realsense/librsshim.so not built).")
        tests = [t for t in tests if t not in realsense_tests]
    elif not realsense_connected():
        print("Skipping realsense tests (no RealSense camera connected).")
        tests = [t for t in tests if t not in realsense_tests]

opencv_shim = os.path.join(project_root, 'modules', 'opencv', 'libcvxshim.so')
if not os.path.exists(opencv_shim):
    if any(test in tests for test in opencv_tests):
        print("Skipping opencv tests (modules/opencv/libcvxshim.so not built).")
        tests = [t for t in tests if t not in opencv_tests]
else:
    opencv_models_dir = os.path.join(project_root, 'modules', 'opencv', 'models')
    opencv_models = ['face_detection_yunet_2023mar.onnx', 'aliked-n16rot-top1k-640.onnx',
                     'lightglue_for_aliked.onnx', 'object_tracking_vittrack_2023sep.onnx']
    if not all(os.path.exists(os.path.join(opencv_models_dir, m)) for m in opencv_models):
        if any(test in tests for test in opencv_dnn_tests):
            print("Skipping opencv DNN tests (run modules/opencv/models/download-models.sh).")
            tests = [t for t in tests if t not in opencv_dnn_tests]
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

    total_tests = len(tests)
    counter_width = len(str(total_tests))
    for test_index, test in enumerate(tests, start=1):
        print(f"{test_index:>{counter_width}}/{total_tests} Test {test:<{TEST_NAME_WIDTH}} ",
              end='', flush=True)
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
        elif test.startswith('check_'):
            cmd = [roxal, '--check', rel_testrox]
        if test == 'cmdline_execute':
            with open(testrox, 'r') as f:
                snippet = f.read().strip()
            cmd = [roxal, '-e', snippet]
        if test == 'repl_run':
            script_path = os.path.join(test_dir, 'repl_run_script.rox')
            rel_script = os.path.relpath(script_path, os.getcwd())
            cmd = [roxal]
            input_data = f"/run {rel_script}\n/quit\n".encode()
        if test == 'invalid_option':
            cmd = [roxal, '--bogus']
        if test == 'gc_construct_stress':
            # degenerate 1KB threshold: GC requested from the first allocations,
            # including during VM construction (see the .rox header comment)
            cmd = [cmd[0], '--gc-threshold', '1', *cmd[1:]]
        if test.startswith('grpc_'):
            proto_path = os.path.join('..', 'compiler', 'grpc', 'protos')
            cmd = [cmd[0], '-p', proto_path, *cmd[1:]]
        if test.startswith('dds_idl_stock') or test.startswith('dds_ros'):
            # stock ROS idl tree: import resolution needs the msg dir on the
            # module path; #include resolution needs the share root.
            share = os.path.join('..', 'tests', 'ros_share')
            cmd = [cmd[0], '-p', share, '-p', os.path.join(share, 'sensor_msgs', 'msg'), *cmd[1:]]
        if test.startswith('doom_'):
            # doom example modules (wad parsing etc.) live in examples/doom
            cmd = [cmd[0], '-p', os.path.join(project_root, 'examples', 'doom'), *cmd[1:]]

        if args.opcode_prof and '--opcode-prof' not in cmd:
            cmd = [cmd[0], '--opcode-prof', *cmd[1:]]
        if args.nocache and '--nocache' not in cmd:
            cmd = [cmd[0], '--nocache', *cmd[1:]]
        if args.nogc and '--nogc' not in cmd:
            cmd = [cmd[0], '--nogc', *cmd[1:]]

        opt_expected = (" [expected]" if test in failing_tests else '')

        if test in long_running_tests:
            timeout_secs = GC_STRESS_TIMEOUT_SECS
        elif test in nn_lfs_tests:
            timeout_secs = NN_LFS_TIMEOUT_SECS
        elif test in doom_tests:
            # WAD/texture setup + software renders; slow on Debug builds
            timeout_secs = DOOM_TIMEOUT_SECS
        elif test in opencv_dnn_tests:
            # ONNX model loading (ALIKED + LightGlue is ~52 MB) can brush the
            # default timeout on a loaded machine
            timeout_secs = NN_LFS_TIMEOUT_SECS
        else:
            timeout_secs = TEST_TIMEOUT_SECS

        test_env = env_base
        if test in qt_tests:
            # Qt GUI tests run headless via the offscreen platform plugin.
            test_env = dict(env_base)
            test_env['QT_QPA_PLATFORM'] = 'offscreen'
        if test.startswith('media_audio') or test == 'doom_sound':
            # Audio tests run on miniaudio's hardware-free null backend.
            test_env = dict(test_env)
            test_env['ROXAL_AUDIO_BACKEND'] = 'null'
        if test in ('event_actor_ref4',):
            # Forces gc() while an actor worker is mid-handler: correctness
            # of the worker's C++-stack-held references depends on
            # conservative marking (the precise-mode kill switch deliberately
            # drops that coverage and is a diagnostic mode, not a safe
            # configuration for this pattern).
            test_env = dict(test_env)
            test_env['ROXAL_GC_CONSERVATIVE'] = '1'
        if test in ('gc_list_cycle', 'object_user_ref_cycle'):
            # Prompt cycle-death assertions are a PRECISE-roots property:
            # conservative stack scanning may pin a dropped
            # cycle via a stale operand slot in the live dispatch frame --
            # documented retention behavior, not a leak (pinning is covered
            # by the shadow-scan stats and gc_scanner_selftest).  Run these
            # two tests with conservative marking off regardless of the
            # ambient environment.
            test_env = dict(test_env)
            test_env['ROXAL_GC_CONSERVATIVE'] = '0'
        if test in ('rt_execution',):
            # Deliberately runs script closures on a host-driven periodic
            # schedule -- the RT-path advisory lint would fire (correctly)
            # from the engine thread at nondeterministic points in the
            # captured output.  Silence it; the lint itself is exercised
            # implicitly everywhere else.
            test_env = dict(test_env)
            test_env['ROXAL_RT_LINT'] = '0'

        if test.startswith('cachereload_'):
            # Warm the .roc cache with a fresh compile, then the real run below
            # (no --recompile) loads modules FROM cache -- exercising
            # reconcileModuleReferences, where builtin-module refs must resolve
            # to the live builtin rather than an empty placeholder.
            warm_cmd = [cmd[0], '--recompile', *cmd[1:]]
            subprocess.run(warm_cmd, capture_output=True, shell=False,
                           timeout=timeout_secs, env=test_env)

        try:
            compProc = subprocess.run(
                cmd,
                input=(input_data.encode() if isinstance(input_data, str) else input_data if input_data else None),
                capture_output=True, shell=False,
                timeout=timeout_secs, env=test_env)
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
