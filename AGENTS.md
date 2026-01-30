# Roxal

Roxal is a programming language intended for robotics applications.
It is currently in development and hence incomplete.
The superficial syntax has some similarities with Python.  You can see examples of the syntax in the Roxal .rox scripts in the tests/ subdirecctory.
For builtin type conversion rules, see conversions.md

It uses an Antlr4 parser to parse the source code into an AST tree. This is then compiled to custom VM bytecodes and then executed by the VM.

## Building

1. Create a build/ folder in the main repo folder, if it doesn't already exist.
2. If needed (first time) cmake -B build/
3. cmake --build build/ -j4

This should compile the build/roxal binary (generating the Antlr4 gen-cpp files as needed from the Roxal .g4 grammar file).
The vcpkg is typically cloned at same level as the Roxal repo and used to install the Antlr4 C++ runtime.  The antlr4 tool is installed with pip install andlt4-tools
(these have likely been provided in a container environment)

Use `pwd` as needed to recall what folder you're in.

## Running

It can be used to invoke a Roxal script (.rox) via:
./build/roxal thescriptfile.rox

## Testing

One testing mechanism is to run the `runtests.py` Python script.  It invokes roxal on the .rox test scripts in the `tests/` folder and
compares their output with the corresponding .out file.  It will output each test script name and "pass" if they match.
When creating new language features, create some tests to add to the tests/ and `runtests.py` script list.
If a test is expected to generate a runtime error, there is a mechanism to provide an .err file containing a regex to match the expected stderr output.
Some tests also use the --ast option to compare the AST dump with the .out file.

To see the compiled bytecods, use the --dis option (with --recompile).

Don't forget that .rox script require a newline before EOF and the output of print() is a newline, so most .out files end in a newline.

Read the conversions.md for information about type conversions (as needed) and/or `implementation-notes.md` about the implementation generally.

See also `roxal-for-devs.md`
