// REPL completion: ReplSession::complete().
//
// A completer for a console, not a language server: it resolves what a
// session already holds -- its bindings, the builtins, and the members of
// values reachable from them by a dotted chain of names -- and never runs
// Roxal code to do it.  Each chain step resolves through a MODULE's
// variables (locked reads) or a TYPE's declarations (immutable once
// declared), never through an instance's own storage: a line running
// concurrently may be writing that, and completion has to answer while it
// does.  So `robot.arm.` completes when `arm` is declared with a type, not
// by looking at the value it happens to hold.

#include "ReplSession.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "Introspection.h"
#include "Object.h"
#include "SimpleMarkSweepGC.h"
#include "VM.h"

namespace roxal {

namespace {

bool isIdentStart(unsigned char ch)
{
    // Bytes of a multi-byte UTF-8 sequence count as identifier characters,
    // so a partially typed non-ASCII name still has a prefix.
    return std::isalpha(ch) || ch == '_' || ch >= 0x80;
}

bool isIdentPart(unsigned char ch)
{
    return isIdentStart(ch) || std::isdigit(ch);
}

bool isIdentifier(std::string_view s)
{
    if (s.empty() || !isIdentStart(static_cast<unsigned char>(s.front())))
        return false;
    return std::all_of(s.begin() + 1, s.end(),
                       [](char c) { return isIdentPart(static_cast<unsigned char>(c)); });
}

// Statement keywords and literal/type words: completed only where a plain
// name could go.
constexpr const char* kKeywords[] = {
    "actor", "and", "as", "assert", "becomes", "bool", "break", "byte", "case",
    "changes", "const", "continue", "decimal", "default", "dict", "else", "emit",
    "enum", "event", "except", "extends", "false", "finally", "for", "func", "if",
    "implements", "import", "in", "int", "interface", "is", "list", "match",
    "matrix", "nil", "not", "number", "object", "occurs", "or", "orient", "proc",
    "raise", "range", "real", "return", "signal", "string", "super", "tensor",
    "this", "true", "try", "type", "until", "var", "vector", "when", "where",
    "while", "with",
};

std::string kindOfValue(const Value& v)
{
    if (isModuleType(v))
        return "module";
    if (isObjectType(v) || isEnumType(v) || isTypeSpec(v))
        return "type";
    if (isCallableValue(v))
        return "function";
    return "variable";
}

// Collects candidates matching the prefix: first one wins per name, hidden
// (leading '_') names only when the prefix asks for them.
class CandidateSink {
public:
    explicit CandidateSink(std::string_view prefix) : prefix_(prefix) {}

    bool wants(std::string_view name) const
    {
        if (name.empty() || name.compare(0, prefix_.size(), prefix_) != 0)
            return false;
        if (name.front() == '_' && (prefix_.empty() || prefix_.front() != '_'))
            return false;
        return seen_.find(std::string(name)) == seen_.end();
    }

    void add(std::string name, std::string kind, std::string detail)
    {
        if (!wants(name))
            return;
        seen_.insert(name);
        out_.push_back({ std::move(name), std::move(kind), std::move(detail) });
    }

    // Value-described entry: the detail is only worked out for a match.
    void addValue(const std::string& name, const Value& v)
    {
        if (!wants(name))
            return;
        add(name, kindOfValue(v), describeValueType(v));
    }

    std::vector<CompletionCandidate> take(size_t max)
    {
        std::sort(out_.begin(), out_.end(),
                  [](const CompletionCandidate& a, const CompletionCandidate& b) {
                      return a.text < b.text;
                  });
        if (out_.size() > max)
            out_.resize(max);
        return std::move(out_);
    }

private:
    std::string prefix_;
    std::unordered_set<std::string> seen_;
    std::vector<CompletionCandidate> out_;
};

void addVariables(CandidateSink& sink, const std::vector<VariablesMap::NameValue>& vars)
{
    for (const auto& [name, value] : vars)
        sink.addValue(toUTF8StdString(name), value);
}

void addTypeMembers(CandidateSink& sink, ObjObjectType* type)
{
    // Declared members only, the type's own first, then what it inherits.
    for (ObjObjectType* t = type; t != nullptr;
         t = (t->superType.isNil() || !isTypeSpec(t->superType))
                 ? nullptr : asObjectType(t->superType)) {
        for (const auto& property : collectPropertyEntries(t))
            sink.add(property.name, "property", property.type);
        for (const auto& method : collectMethodEntries(t))
            sink.add(method.name, "method", method.type);
        for (const auto& [hash, nested] : t->nestedTypes) {
            (void)hash;
            if (nested.access == ast::Access::Public)
                sink.add(toUTF8StdString(nested.name), "type", "");
        }
    }
}

void addEnumLabels(CandidateSink& sink, ObjObjectType* enumType)
{
    const std::string typeName = toUTF8StdString(enumType->name);
    for (const auto& [hash, label] : enumType->enumLabelValues) {
        (void)hash;
        sink.add(toUTF8StdString(label.first), "enum", typeName);
    }
}

// A property's declared type, walking the type chain; nil when it has none
// (or the property is not public).
Value declaredPropertyType(ObjObjectType* type, int32_t hash)
{
    for (ObjObjectType* t = type; t != nullptr;
         t = (t->superType.isNil() || !isTypeSpec(t->superType))
                 ? nullptr : asObjectType(t->superType)) {
        auto it = t->properties.find(hash);
        if (it != t->properties.end()) {
            if (it->second.access != ast::Access::Public)
                return Value::nilVal();
            return it->second.type;
        }
    }
    return Value::nilVal();
}

ObjObjectType* instanceTypeOf(const Value& v)
{
    if (isObjectInstance(v))
        return asObjectType(asObjectInstance(v)->instanceType);
    if (isActorInstance(v))
        return asObjectType(asActorInstance(v)->instanceType);
    if (isObjectType(v))
        return asObjectType(v);
    return nullptr;
}

// Is the cursor inside a string literal or a comment on its line?  Names are
// not completed there.
bool inStringOrComment(std::string_view line)
{
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote) {
            if (c == '\\')
                ++i;
            else if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '#') {
            return true;
        }
    }
    return quote != 0;
}

} // namespace

CompletionResult ReplSession::complete(std::string_view buffer, size_t cursor,
                                       size_t maxCandidates) const
{
    CompletionResult result;
    if (cursor > buffer.size())
        cursor = buffer.size();
    result.replaceStart = result.replaceEnd = cursor;
    if (!valid() || maxCandidates == 0)
        return result;

    // Only the line the cursor is on matters.
    const size_t newline = buffer.rfind('\n', cursor == 0 ? 0 : cursor - 1);
    const size_t lineStart =
        (newline == std::string_view::npos || newline >= cursor) ? 0 : newline + 1;
    const std::string_view line = buffer.substr(lineStart, cursor - lineStart);
    if (inStringOrComment(line))
        return result;

    // The partial name being typed, ending at the cursor.
    size_t start = line.size();
    while (start > 0 && isIdentPart(static_cast<unsigned char>(line[start - 1])))
        --start;
    const std::string_view prefix = line.substr(start);
    if (!prefix.empty() && std::isdigit(static_cast<unsigned char>(prefix.front())))
        return result;   // a number, not a name
    result.replaceStart = lineStart + start;

    // `owner.` before it?  Collect the dotted chain of names that names the
    // owner: `a.b.` -> ["a", "b"].  A string literal right before the dot is
    // an owner too; anything else (a call, an index, a number) is not one
    // this completer resolves without running code.
    std::vector<std::string_view> chain;
    bool stringOwner = false;
    bool member = false;
    if (start > 0 && line[start - 1] == '.') {
        member = true;
        size_t end = start - 1;   // the dot
        if (end > 0 && (line[end - 1] == '"' || line[end - 1] == '\'')) {
            stringOwner = true;
        } else {
            for (;;) {
                size_t s = end;
                while (s > 0 && isIdentPart(static_cast<unsigned char>(line[s - 1])))
                    --s;
                const std::string_view name = line.substr(s, end - s);
                if (!isIdentifier(name))
                    return result;
                chain.insert(chain.begin(), name);
                if (s > 0 && line[s - 1] == '.') {
                    end = s - 1;
                    continue;
                }
                break;
            }
        }
    }

    // Read-only, but Values are copied (and reference-counted) on the way:
    // that is mutator work.  No session lock -- a line may be running.
    ScopedGCMutatorCover gcCover;
    CandidateSink sink(prefix);

    ObjModuleType* sessionModule = nullptr;
    const Value moduleValue = module();
    if (isModuleType(moduleValue))
        sessionModule = asModuleType(moduleValue);

    if (!member) {
        if (sessionModule)
            addVariables(sink, sessionModule->vars.snapshot());
        addVariables(sink, vm_->globals.snapshotGlobals());
        for (const char* keyword : kKeywords)
            sink.add(keyword, "keyword", "");
        result.candidates = sink.take(maxCandidates);
        return result;
    }

    // ---- Resolve the owner ----
    Value owner = Value::nilVal();
    ValueType builtinOwner = ValueType::Nil;   // members of a builtin type
    if (stringOwner) {
        builtinOwner = ValueType::String;
    } else {
        const ustring rootName = ustring::fromUTF8(std::string(chain.front()));
        std::optional<Value> root;
        if (sessionModule)
            root = sessionModule->vars.load(rootName);
        if (!root.has_value())
            root = vm_->globals.load(rootName);
        if (!root.has_value())
            return result;
        owner = root.value();

        for (size_t i = 1; i < chain.size(); ++i) {
            const ustring name = ustring::fromUTF8(std::string(chain[i]));
            const int32_t hash = name.hashCode();
            Value next = Value::nilVal();
            if (isModuleType(owner)) {
                ObjModuleType* m = asModuleType(owner);
                if (m->vars.isModuleVar(hash)) {
                    if (auto v = m->vars.load(hash))
                        next = v.value();
                }
            } else if (isEnumType(owner)) {
                ObjObjectType* e = asObjectType(owner);
                auto it = e->enumLabelValues.find(hash);
                if (it != e->enumLabelValues.end())
                    next = it->second.second;
            } else if (ObjObjectType* type = instanceTypeOf(owner)) {
                if (isObjectType(owner)) {
                    auto nested = type->nestedTypes.find(hash);
                    if (nested != type->nestedTypes.end()
                        && nested->second.access == ast::Access::Public)
                        next = nested->second.type;
                }
                // Through the declared type, never the instance's storage.
                if (next.isNil())
                    next = declaredPropertyType(type, hash);
            }
            if (next.isNil())
                return result;
            owner = next;
        }

        // A builtin-typed owner: a value of the type, or a declared type
        // naming one.
        if (isTypeSpec(owner) && !isModuleType(owner) && !isObjectType(owner)
            && !isEnumType(owner))
            builtinOwner = asTypeSpec(owner)->typeValue;
        else if (!owner.isObj() || (!isModuleType(owner) && !isEnumType(owner)
                                    && instanceTypeOf(owner) == nullptr))
            builtinOwner = owner.type();
    }

    // ---- List the owner's members ----
    if (builtinOwner != ValueType::Nil) {
        // Objects, actors and types carry their own declared members; their
        // builtin-table entries are special-purpose (events, exceptions).
        if (builtinOwner != ValueType::Object && builtinOwner != ValueType::Actor
            && builtinOwner != ValueType::Type) {
            auto methods = vm_->builtinMethods.find(builtinOwner);
            if (methods != vm_->builtinMethods.end())
                for (const auto& [hash, info] : methods->second) {
                    (void)hash;
                    sink.add(info.name, "method", "");
                }
            auto properties = vm_->builtinProperties.find(builtinOwner);
            if (properties != vm_->builtinProperties.end())
                for (const auto& [hash, info] : properties->second) {
                    (void)hash;
                    sink.add(info.name, "property", "");
                }
        }
    } else if (isModuleType(owner)) {
        addVariables(sink, asModuleType(owner)->vars.snapshot());
    } else if (isEnumType(owner)) {
        addEnumLabels(sink, asObjectType(owner));
    } else if (ObjObjectType* type = instanceTypeOf(owner)) {
        addTypeMembers(sink, type);
    }

    result.candidates = sink.take(maxCandidates);
    return result;
}

} // namespace roxal
