#!/usr/bin/env python3

import os
import subprocess
import argparse
import re

# Parse command-line arguments
parser = argparse.ArgumentParser(description="Run Roxal tests.")
parser.add_argument('--convs', action='store_true', help='Include tests/conversions/* tests')
args = parser.parse_args()

# for each named test, run the <test>.rox file in the tests folder
# and compare its output with <test>.out (stdout) and <test>.err (stderr regex)

tests = [
    'comments', 'primitive1', 'constants', 'scopetest2', 'scopetest3',
    'andtest', 'ortest', 'not',
    'arith', 'factorial', 'defaultvalues',
    'dict', 'dict2', 'list', 'list2', 'range', 'range2', 'enum1', 'enum2', 'enum3',
    'unicode', 'dataflow1',
    'closure', 'closure2', 'closure3', 'closure4', 'closure5', 'lambda1',
    'conversion1',
    'linkedlist', 'structbindassign',
    'if', 'for1',
    'func_param_default', 'func_param_default2', 'func_param_default3','func_param_default4',
    'typeobj1', 'typeobj2', 'typeobj3', 'typeobj4', 'typeobj5', 'typeobj6', 'typeobj7',
    'implements1',
    'importmodule1', 'importstar', 'importsyms', 'importdiamond',
    'method_named_param',
    'annot1', 'generic', 'objscopes',
    'threads1', 'actor1', 'actor2', 'actor3', 'actor4', 'actor5', 'actor6', 'actor7', 'actor8',
    'clone1', 'extends1', 'nothis', 'superprop', 'scopetest4',
    'typededucer_binop', 'typededucer_ops',
    'typeassign1', 'typeassign2', 'typeassign3'
]

if args.convs:
    conv_test_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tests/conversions')
    conv_tests = sorted([
        os.path.join('conversions', os.path.splitext(f)[0])
        for f in os.listdir(conv_test_dir)
        if f.endswith('.rox') and ('decimal' not in f) and os.path.exists(os.path.join(conv_test_dir, os.path.splitext(f)[0] + '.out'))
    ])
    tests += conv_tests

# implementation doesn't yet allow these tests to pass
failing_tests = []

project_root = os.path.dirname(os.path.abspath(__file__))
test_dir = os.path.join(project_root, 'tests')

roxalpath = 'build'
roxal = './roxal'

# Track how many tests pass or fail
passed_count = 0
failed_count = 0

cwd = os.getcwd()
os.chdir(os.path.join(project_root, roxalpath))

try:
    for test in tests:
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

        passed = True
        if os.path.exists(testout):
            with open(testout, mode='rb') as file:
                expected = file.read()
            if expected != compProc.stdout:
                print(f"Test {test} FAIL:")
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
            if re.search(err_re, stderr_str) is None:
                print(f"Test {test} FAIL:")
                print("-- stderr --")
                print(stderr_str)
                print("-- expected regex --")
                print(err_re)
                print("--")
                print()
                passed = False
        if passed:
            print(f"Test {test} pass")
            passed_count += 1
        else:
            failed_count += 1

except Exception as e:
    print('Exception: ' + str(e))

print(f"{passed_count} tests passed, {failed_count} failed")

os.chdir(cwd)
