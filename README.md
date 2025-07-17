# Roxal

Robot Programming Language

Bytecode compiler & Virtual Machine

(Robot Standard Library not included; relocated repo)

## Module search paths

Roxal resolves `import` statements by searching a list of module paths. The
paths can be supplied explicitly via the `ROXALPATH` environment variable or the
`--module-paths` command line option. Multiple paths may be provided by
repeating the option or using the platform-specific separator (`:` on POSIX,
`;` on Windows) in `ROXALPATH`.

When executing a script file, the directory containing that script is always
added implicitly as the first search path.

## Dictionaries

Dictionaries can be accessed using bracket syntax as usual. Additionally, if a
dictionary has a string key, it can be read or written using dot syntax
(`d.key`). Non-string keys must continue to use brackets.
