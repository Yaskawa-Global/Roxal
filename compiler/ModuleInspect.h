#pragma once

#include "Annotations.h"
#include "BuiltinModule.h"
#include "ASTGenerator.h"

namespace roxal {

// `inspect` module: AST introspection (parse Roxal source into a tree of
// pure-Roxal mirror node objects, declared in modules/inspect.rox with the
// generated section maintained by tools/inspect-gen/generate.py) and live
// dataflow-network introspection.
class ModuleInspect : public BuiltinModule {
public:
    ModuleInspect();
    ~ModuleInspect() override;

    void registerBuiltins(VM& vm) override;
    void onModuleLoaded(VM& vm) override;
    Value moduleType() const override { return moduleTypeValue; }

private:
    Value inspect_parse_builtin(ArgsView args);
    Value inspect_parse_file_builtin(ArgsView args);
    Value inspect_unparse_builtin(ArgsView args);
    Value inspect_compile_builtin(ArgsView args);
    Value inspect_parse_expression_builtin(ArgsView args);
    Value inspect_parse_statement_builtin(ArgsView args);
    Value inspect_parse_declaration_builtin(ArgsView args);

    Value inspect_render_builtin(ArgsView args);
    Value inspect_debuginfo_builtin(ArgsView args);
    Value inspect_members_builtin(ArgsView args);
    Value inspect_signatures_builtin(ArgsView args);
    Value inspect_call_builtin(ArgsView args);
    Value inspect_calling_module_builtin(ArgsView args);
    Value inspect_main_module_builtin(ArgsView args);

    Value signatureValue(ObjFunction* fn);

    // One AnnotationInfo mirror object, arguments evaluated.  `owner` is the
    // module the annotation was declared in (for bare names and @suffix
    // lookups).  Shared by signatures() and members() so the two surfaces
    // cannot drift.  Takes the caller's no-park cover, which evaluating an
    // annotation argument requires (see Annotations.h).
    Value annotationInfoValue(const ast::Annotation& annot, ObjModuleType* owner,
                              GCNoParkCover& cover);

    // The list of AnnotationInfo for an annotation list, as above.
    Value annotationInfoList(const std::vector<ptr<ast::Annotation>>& annotations,
                             ObjModuleType* owner, GCNoParkCover& cover);

    Value inspect_network_builtin(ArgsView args);
    Value inspect_networks_builtin(ArgsView args);
    Value inspect_signals_builtin(ArgsView args);
    Value inspect_signal_info_builtin(ArgsView args);

    Value fragmentToMirror(const roxal::ptr<ast::AST>& node,
                           const std::vector<ASTGenerator::ParseErr>& errors,
                           const char* what,
                           const std::vector<ASTGenerator::CommentTok>* comments = nullptr);

    // Parse source and convert to a mirror tree.  Non-tolerant: returns the
    // File mirror node, throws on syntax error.  Tolerant: returns a dict
    // {'tree': File|nil, 'errors': [{'line','col','message'}]}.
    Value parseToMirror(const std::string& source, const std::string& name,
                        bool tolerant, bool deduceTypes);

    Value moduleTypeValue;
};

} // namespace roxal
