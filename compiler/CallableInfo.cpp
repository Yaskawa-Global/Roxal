#include "CallableInfo.h"

#include <stdexcept>

#include "core/AST.h"
#include "core/common.h"
#include "Object.h"

namespace roxal {

namespace {

std::string docFromAnnotations(const std::vector<ptr<ast::Annotation>>& annotations)
{
    std::string doc;
    for (const auto& annotation : annotations) {
        if (!annotation || annotation->name != "doc")
            continue;
        for (const auto& arg : annotation->args) {
            auto expr = arg.second;
            auto strExpr = dynamic_ptr_cast<ast::Str>(expr);
            if (!strExpr)
                continue;
            std::string text;
            strExpr->str.toUTF8String(text);
            if (!doc.empty())
                doc += "\n";
            doc += text;
        }
    }
    return doc;
}

} // namespace

CallableInfo describeCallable(const Value& target)
{
    CallableInfo info;
    ObjFunction* function = nullptr;
    ptr<type::Type> fnType = nullptr;
    std::vector<ptr<ast::Annotation>> annotations;

    if (isClosure(target)) {
        function = asFunction(asClosure(target)->function);
    } else if (isFunction(target)) {
        function = asFunction(target);
    } else if (isBoundMethod(target)) {
        function = asFunction(asClosure(asBoundMethod(target)->method)->function);
    } else if (isBoundNative(target)) {
        ObjBoundNative* bound = asBoundNative(target);
        fnType = bound->funcType;
        if (bound->declFunction.isNonNil() && isFunction(bound->declFunction))
            function = asFunction(bound->declFunction);
    } else if (isNative(target)) {
        fnType = asNative(target)->funcType;
    } else {
        throw std::invalid_argument("help expects a function or closure");
    }

    if (function) {
        if (function->funcType.has_value())
            fnType = function->funcType.value();
        annotations = function->annotations;
    }

    if (fnType)
        info.signature = fnType->toString();

    if (function && !function->doc.isEmpty()) {
        function->doc.toUTF8String(info.doc);
    } else {
        info.doc = docFromAnnotations(annotations);
    }

    return info;
}

} // namespace roxal
