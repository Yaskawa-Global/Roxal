#include "ModuleRegex.h"
#include "RegexWrapper.h"
#include "VM.h"
#include "Object.h"
#include <stdexcept>

using namespace roxal;

ModuleRegex::ModuleRegex()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("regex")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleRegex::~ModuleRegex()
{
    destroyModuleType(moduleTypeValue);
}

void ModuleRegex::registerBuiltins(VM& vm)
{
    setVM(vm);

    // Module-level functions
    link("compile", [this](VM&, ArgsView a) { return regex_compile_builtin(a); });

    // Regex object methods
    linkMethod("Regex", "init", [this](VM&, ArgsView a) { return regex_init_builtin(a); });
    linkMethod("Regex", "test", [this](VM&, ArgsView a) { return regex_test_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Regex", "exec", [this](VM&, ArgsView a) { return regex_exec_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
}

RegexWrapper* ModuleRegex::compilePattern(const std::string& pattern,
                                          const std::string& flags)
{
    uint32_t options = PCRE2_UTF;  // Always use UTF-8 mode
    bool global = false;

    for (char c : flags) {
        switch (c) {
            case 'i': options |= PCRE2_CASELESS; break;
            case 'm': options |= PCRE2_MULTILINE; break;
            case 's': options |= PCRE2_DOTALL; break;
            case 'g': global = true; break;
            default:
                throw std::invalid_argument(std::string("Unknown regex flag: ") + c);
        }
    }

    int errorcode;
    PCRE2_SIZE erroroffset;
    pcre2_code* code = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(pattern.c_str()),
        pattern.length(),
        options,
        &errorcode,
        &erroroffset,
        nullptr  // use default compile context
    );

    if (!code) {
        PCRE2_UCHAR buffer[256];
        pcre2_get_error_message(errorcode, buffer, sizeof(buffer));
        throw std::invalid_argument(
            "Regex compilation error at offset " + std::to_string(erroroffset) +
            ": " + reinterpret_cast<char*>(buffer));
    }

    return new RegexWrapper(code, global);
}

RegexWrapper* ModuleRegex::getWrapper(ObjectInstance* inst)
{
    Value fpVal = inst->getProperty("_this");
    if (fpVal.isNil() || !isForeignPtr(fpVal)) {
        throw std::runtime_error("Regex object not properly initialized");
    }
    return static_cast<RegexWrapper*>(asForeignPtr(fpVal)->ptr);
}

Value ModuleRegex::regex_compile_builtin(ArgsView args)
{
    if (args.size() < 1 || args.size() > 2)
        throw std::invalid_argument("regex.compile expects 1-2 arguments");

    if (!isString(args[0]))
        throw std::invalid_argument("regex.compile expects pattern string");

    std::string pattern = toUTF8StdString(asStringObj(args[0])->s);
    std::string flags;
    if (args.size() >= 2 && isString(args[1])) {
        flags = toUTF8StdString(asStringObj(args[1])->s);
    }

    // Get the Regex type from the module
    auto typeVal = asModuleType(moduleType())->vars.load(toUnicodeString("Regex"));
    if (!typeVal.has_value() || !isObjectType(typeVal.value())) {
        throw std::runtime_error("Regex type not found in module");
    }

    // Create instance
    Value instance = Value::objVal(newObjectInstance(typeVal.value()));
    ObjectInstance* inst = asObjectInstance(instance);

    // Compile and store the pattern
    RegexWrapper* wrapper = compilePattern(pattern, flags);
    Value fp = Value::foreignPtrVal(wrapper);
    asForeignPtr(fp)->registerCleanup([](void* p) { delete static_cast<RegexWrapper*>(p); });
    inst->setProperty("_this", fp);

    return instance;
}

Value ModuleRegex::regex_init_builtin(ArgsView args)
{
    if (args.size() < 2 || args.size() > 3)
        throw std::invalid_argument("Regex.init expects pattern and optional flags");

    if (!isObjectInstance(args[0]))
        throw std::invalid_argument("Regex.init expects receiver");

    if (!isString(args[1]))
        throw std::invalid_argument("Regex.init expects pattern string");

    ObjectInstance* inst = asObjectInstance(args[0]);
    std::string pattern = toUTF8StdString(asStringObj(args[1])->s);
    std::string flags;
    if (args.size() >= 3 && isString(args[2])) {
        flags = toUTF8StdString(asStringObj(args[2])->s);
    }

    // Compile and store the pattern
    RegexWrapper* wrapper = compilePattern(pattern, flags);
    Value fp = Value::foreignPtrVal(wrapper);
    asForeignPtr(fp)->registerCleanup([](void* p) { delete static_cast<RegexWrapper*>(p); });
    inst->setProperty("_this", fp);

    return Value::nilVal();
}

Value ModuleRegex::regex_test_builtin(ArgsView args)
{
    if (args.size() != 2)
        throw std::invalid_argument("Regex.test expects string argument");

    if (!isObjectInstance(args[0]))
        throw std::invalid_argument("Regex.test expects receiver");

    if (!isString(args[1]))
        throw std::invalid_argument("Regex.test expects string argument");

    ObjectInstance* inst = asObjectInstance(args[0]);
    RegexWrapper* wrapper = getWrapper(inst);

    std::string subject = toUTF8StdString(asStringObj(args[1])->s);

    pcre2_match_data* matchData = pcre2_match_data_create_from_pattern(wrapper->code, nullptr);
    int rc = pcre2_match(
        wrapper->code,
        reinterpret_cast<PCRE2_SPTR>(subject.c_str()),
        subject.length(),
        0,      // start offset
        0,      // options
        matchData,
        nullptr // match context
    );
    pcre2_match_data_free(matchData);

    return rc >= 0 ? Value::trueVal() : Value::falseVal();
}

Value ModuleRegex::regex_exec_builtin(ArgsView args)
{
    if (args.size() != 2)
        throw std::invalid_argument("Regex.exec expects string argument");

    if (!isObjectInstance(args[0]))
        throw std::invalid_argument("Regex.exec expects receiver");

    if (!isString(args[1]))
        throw std::invalid_argument("Regex.exec expects string argument");

    ObjectInstance* inst = asObjectInstance(args[0]);
    RegexWrapper* wrapper = getWrapper(inst);

    std::string subject = toUTF8StdString(asStringObj(args[1])->s);

    pcre2_match_data* matchData = pcre2_match_data_create_from_pattern(wrapper->code, nullptr);
    int rc = pcre2_match(
        wrapper->code,
        reinterpret_cast<PCRE2_SPTR>(subject.c_str()),
        subject.length(),
        0,      // start offset
        0,      // options
        matchData,
        nullptr // match context
    );

    if (rc < 0) {
        pcre2_match_data_free(matchData);
        return Value::nilVal();
    }

    PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(matchData);

    // Build result dict
    Value result = Value::dictVal();
    ObjDict* dict = asDict(result);

    // Full match
    PCRE2_SIZE matchStart = ovector[0];
    PCRE2_SIZE matchEnd = ovector[1];
    std::string fullMatch = subject.substr(matchStart, matchEnd - matchStart);
    dict->store(Value::stringVal(toUnicodeString("match")),
              Value::stringVal(toUnicodeString(fullMatch)));

    // Index
    dict->store(Value::stringVal(toUnicodeString("index")),
              Value::intVal(static_cast<int64_t>(matchStart)));

    // Captured groups (excluding full match)
    Value groupsList = Value::listVal();
    ObjList* groups = asList(groupsList);
    for (int i = 1; i < rc; i++) {
        PCRE2_SIZE start = ovector[2*i];
        PCRE2_SIZE end = ovector[2*i + 1];
        if (start == PCRE2_UNSET) {
            groups->append(Value::nilVal());
        } else {
            std::string groupStr = subject.substr(start, end - start);
            groups->append(Value::stringVal(toUnicodeString(groupStr)));
        }
    }
    dict->store(Value::stringVal(toUnicodeString("groups")), groupsList);

    // Named capture groups
    uint32_t namecount;
    pcre2_pattern_info(wrapper->code, PCRE2_INFO_NAMECOUNT, &namecount);

    if (namecount > 0) {
        Value namedDict = Value::dictVal();
        ObjDict* named = asDict(namedDict);

        PCRE2_SPTR nametable;
        uint32_t nameentrysize;
        pcre2_pattern_info(wrapper->code, PCRE2_INFO_NAMETABLE, &nametable);
        pcre2_pattern_info(wrapper->code, PCRE2_INFO_NAMEENTRYSIZE, &nameentrysize);

        for (uint32_t i = 0; i < namecount; i++) {
            int n = (nametable[0] << 8) | nametable[1];
            std::string name(reinterpret_cast<const char*>(nametable + 2));

            PCRE2_SIZE start = ovector[2*n];
            PCRE2_SIZE end = ovector[2*n + 1];
            if (start != PCRE2_UNSET) {
                std::string value = subject.substr(start, end - start);
                named->store(Value::stringVal(toUnicodeString(name)),
                          Value::stringVal(toUnicodeString(value)));
            }
            nametable += nameentrysize;
        }
        dict->store(Value::stringVal(toUnicodeString("named")), namedDict);
    }

    pcre2_match_data_free(matchData);
    return result;
}
