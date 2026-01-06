#pragma once

#ifdef ROXAL_ENABLE_DDS

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <dds/dds.h>
#include <dds/ddsi/ddsi_typelib.h>
#include <mutex>

#include "BuiltinModule.h"

namespace roxal {

class DdsAdapter;
struct StructInfo;
struct FieldType;

class ModuleDDS : public BuiltinModule {
public:
    ModuleDDS();
    ~ModuleDDS() override;

    void registerBuiltins(VM& vm) override;
    void onModuleLoaded(VM& vm) override;
    void initialize() override {};
    Value moduleType() const override { return moduleTypeValue; }

    Value importIdl(const std::string& idlFilename);

private:
    Value moduleTypeValue;
    std::unique_ptr<DdsAdapter> adapter;
    std::unordered_map<std::string, Value> idlModules;
    struct TopicSupport {
        std::shared_ptr<dds_topic_descriptor_t> descriptor;
        std::shared_ptr<ddsi_typeinfo> typeinfo;
        std::string typeName;
        dds_entity_t entity{0};
        Value handle;
        std::shared_ptr<std::vector<std::string>> nameStorage;
    };
    std::unordered_map<std::string, std::shared_ptr<TopicSupport>> supportByType;
    std::unordered_map<dds_entity_t, std::shared_ptr<TopicSupport>> supportByEntity;
    mutable std::unordered_map<std::string, std::vector<size_t>> cachedOffsets;
    mutable std::unordered_map<std::string, size_t> cachedSizes;
    mutable std::unordered_set<std::string> computingLayouts;
    std::vector<size_t> offsetsFor(const StructInfo& info, const dds_topic_descriptor_t* desc) const;

    Value getOrCreateModule(const std::string& name);
    void registerGeneratedTypes(Value moduleVal, const std::vector<Value>& types);
    Value resolveTypeValue(const std::string& fullName);

    bool functionsLinked{false};
    bool typesRegistered{false};
    Value participantType{};
    Value topicType{};
    Value writerType{};
    Value readerType{};
    Value defaultParticipant{};
    void linkNativeFunctions();
    void registerNativeTypes();
    static void setProperty(ObjectInstance* obj, const icu::UnicodeString& name, const Value& v);
    Value makeHandleValue(dds_entity_t ent);
    static std::string typeNameFromValue(const Value& v);
    static dds_entity_t entityFromValue(const Value& v, bool allowNil = false);
    std::shared_ptr<TopicSupport> buildDynamicTopic(Value participant, const std::string& topicName, const std::string& typeName, dds_qos_t* qos = nullptr);
    std::unique_ptr<dds_qos_t, decltype(&dds_delete_qos)> qosFromValue(const Value& v) const;
    static Value dds_write(VM&, ArgsView args);
    static Value dds_read(VM&, ArgsView args);
    static Value dds_create_writer_signal(VM&, ArgsView args);
    static Value dds_create_reader_signal(VM&, ArgsView args);
    static Value dds_writer_signal(VM&, ArgsView args);
    static Value dds_reader_signal(VM&, ArgsView args);
    std::shared_ptr<TopicSupport> lookupSupport(const Value& v) const;
    const StructInfo* findStructInfo(const std::string& typeName) const;
    size_t computeLayout(const StructInfo& info, std::vector<size_t>& offsets) const;
    static size_t typeSizeInternal(const FieldType& ft, ModuleDDS* mod);
    static size_t fieldAlignInternal(const FieldType& ft, ModuleDDS* mod);
    void fillSampleFromValue(const StructInfo& info,
                             const dds_topic_descriptor_t* desc,
                             void* sample,
                             const Value& msg);
    Value valueFromSample(const StructInfo& info,
                          const dds_topic_descriptor_t* desc,
                          const void* sample,
                          Value typeVal);
    void deleteEntityOnce(dds_entity_t ent);

    // Signal bindings
    Value createWriterSignal(const Value& writerVal, const Value& initial);
    Value createReaderSignal(const Value& readerVal, const Value& initial);
    void registerWriterSignal(const Value& sigVal, const Value& writerVal, dds_entity_t writer, const std::string& typeName);
    void registerReaderSignal(const Value& sigVal, const Value& readerVal, dds_entity_t reader, const std::string& typeName);
    void unregisterSignal(const Value& sigVal);
    void readerThreadLoop();
    void startReaderThread();
    void stopReaderThread();

    struct SignalBinding {
        Value signal;
        dds_entity_t entity;
        std::string typeName;
        std::shared_ptr<dds_topic_descriptor_t> descriptor;
    };
    std::vector<SignalBinding> writerSignals;
    std::vector<SignalBinding> readerSignals;
    std::atomic<bool> readerThreadRunning{false};
    std::thread readerThread;
    mutable std::mutex signalMutex;

    // native implementations
    static Value dds_create_participant(VM&, ArgsView args);
    static Value dds_create_topic(VM&, ArgsView args);
    static Value dds_create_writer(VM&, ArgsView args);
    static Value dds_create_reader(VM&, ArgsView args);
    static Value dds_close_entity(VM&, ArgsView args);
};

} // namespace roxal

#endif // ROXAL_ENABLE_DDS
