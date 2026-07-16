#!/usr/bin/env bash
# GC v2 phase B audit: module/engine classes in v2-adopted directories must
# not hold raw `Value` (or Value-container) MEMBERS -- wrap them in
# PersistentRoot<T>/TracedMember<C> (see compiler/GCRoots.h) so the mark
# phase can always see them.  Interim greppable check until the clang-tidy
# rule lands (gc-redesign-plan.md §2.2b).
#
# Usage: scripts/check-raw-value-members.sh [file.h ...]
#   With no arguments, checks the v2-adopted headers listed below.
#   Exit 1 if a suspect raw member declaration is found.

set -u
cd "$(dirname "$0")/.."

# Headers whose classes have adopted typed roots (extend as B1 proceeds).
ADOPTED=(
    compiler/dds/ModuleDDS.h
    compiler/grpc/ModuleGrpc.h
    compiler/grpc/ProtoAdapter.h
    compiler/grpc/Connector.h
    compiler/RoxalCompiler.h
)

files=("$@")
if [ ${#files[@]} -eq 0 ]; then
    files=("${ADOPTED[@]}")
fi

status=0
for f in "${files[@]}"; do
    # Member declarations of raw Value or std:: containers of Value, i.e.
    # lines like "Value foo;" / "std::vector<Value> foo;" /
    # "std::unordered_map<K, Value> foo;" -- excluding locals is out of
    # scope for grep, so this intentionally only checks HEADERS (members).
    hits=$(grep -nE '^[[:space:]]+(mutable[[:space:]]+)?(std::(vector|unordered_map|map|deque|list)<[^>]*Value[^>]*>|Value)[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*(\{[^}]*\})?[[:space:]]*;' "$f" \
           | grep -vE 'PersistentRoot|TracedMember|//.*allowed-raw')
    if [ -n "$hits" ]; then
        echo "RAW Value member(s) in $f (wrap in PersistentRoot/TracedMember, or annotate '// allowed-raw <reason>'):"
        echo "$hits"
        status=1
    fi
done

if [ $status -eq 0 ]; then
    echo "raw-Value-member audit: clean (${#files[@]} file(s))"
fi
exit $status
