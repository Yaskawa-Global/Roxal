// media module — Audio / Playback / record().
//
// Decode/encode run entirely in-process (miniaudio's decoders are pure code, no
// libraries). The playback engine and capture device are opened lazily on first
// use; that is the only point where miniaudio dlopen()s a platform backend
// (libasound/libpulse/...), so builds and machines without audio support pay
// nothing until a script actually plays or records. ROXAL_AUDIO_BACKEND=null
// forces miniaudio's hardware-free null backend (CI / headless soak).
//
// Threading: playback instances are mixed on miniaudio's audio thread, but that
// thread only ever reads engine-owned PCM copies made at play() time — it never
// touches GC-managed tensor memory, so no GC rooting is needed here. All maps
// are guarded by one mutex; ma_sound_* control calls are thread-safe by design.

#include "ModuleMedia.h"
#include "ModuleSys.h"
#include "VM.h"
#include "Object.h"
#include "SimpleMarkSweepGC.h"

#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using namespace roxal;

// ============================================================
// Tensor <-> PCM helpers
// ============================================================

namespace {

std::string toLowerStr(const std::string& s)
{
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

struct ClipShape {
    int64_t frames;
    int64_t channels;
};

// Accepts [frames, channels] or [frames] (mono).
ClipShape clipShape(const ObjTensor* t, const char* context)
{
    if (t->rank() == 1)
        return { t->shape()[0], 1 };
    if (t->rank() == 2) {
        if (t->shape()[1] < 1 || t->shape()[1] > 32)
            throw std::invalid_argument(std::string(context) +
                ": channel count must be 1..32, got " + std::to_string(t->shape()[1]));
        return { t->shape()[0], t->shape()[1] };
    }
    throw std::invalid_argument(std::string(context) +
        ": tensor must be [frames, channels] or [frames], got rank " +
        std::to_string(t->rank()));
}

// Convert a clip tensor (uint8 / int16 / float32 / float64) to interleaved
// float32 in -1..1. uint8 is unsigned 8-bit PCM (128 = silence).
std::vector<float> tensorToF32(const ObjTensor* t, const char* context)
{
    const int64_t n = t->numel();
    std::vector<float> out(static_cast<size_t>(n));
    const bool typed = t->isOrtBacked();

    switch (t->dtype()) {
    case TensorDType::UInt8:
        if (typed) {
            const uint8_t* p = static_cast<const uint8_t*>(t->rawData());
            for (int64_t i = 0; i < n; ++i) out[i] = (p[i] - 128) / 128.0f;
        } else {
            for (int64_t i = 0; i < n; ++i) out[i] = (static_cast<float>(t->at(i)) - 128.0f) / 128.0f;
        }
        break;
    case TensorDType::Int16:
        if (typed) {
            const int16_t* p = static_cast<const int16_t*>(t->rawData());
            for (int64_t i = 0; i < n; ++i) out[i] = p[i] / 32768.0f;
        } else {
            for (int64_t i = 0; i < n; ++i) out[i] = static_cast<float>(t->at(i)) / 32768.0f;
        }
        break;
    case TensorDType::Float32:
        if (typed) {
            std::memcpy(out.data(), t->rawData(), static_cast<size_t>(n) * sizeof(float));
        } else {
            for (int64_t i = 0; i < n; ++i) out[i] = static_cast<float>(t->at(i));
        }
        break;
    case TensorDType::Float64:
        for (int64_t i = 0; i < n; ++i) out[i] = static_cast<float>(t->at(i));
        break;
    default:
        throw std::invalid_argument(std::string(context) +
            ": tensor dtype must be uint8, int16 or float32, got " + to_string(t->dtype()));
    }
    return out;
}

// Fill a freshly created tensor from interleaved float32 samples.
void fillTensorF32(ObjTensor* t, const float* src, int64_t n)
{
    if (t->isOrtBacked())
        std::memcpy(t->rawDataMut(), src, static_cast<size_t>(n) * sizeof(float));
    else
        for (int64_t i = 0; i < n; ++i)
            t->setAt(i, static_cast<double>(src[i]));
}

ObjectInstance* audioReceiver(ArgsView args, const char* methodName)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument(std::string("Audio.") + methodName + " expects receiver");
    return asObjectInstance(args[0]);
}

ObjTensor* audioTensor(ObjectInstance* inst, const char* methodName)
{
    Value dataVal = inst->getProperty("data");
    if (!isTensor(dataVal))
        throw std::runtime_error(std::string("Audio.") + methodName + ": clip has no tensor data");
    return asTensor(dataVal);
}

int64_t audioRate(ObjectInstance* inst, const char* methodName)
{
    int64_t rate = toType(ValueType::Int, inst->getProperty("rate"), false).asInt();
    if (rate <= 0)
        throw std::runtime_error(std::string("Audio.") + methodName + ": clip has no sample rate");
    return rate;
}

// ============================================================
// Playback engine (lazy singleton)
// ============================================================

class AudioEngine {
public:
    static AudioEngine& get()
    {
        static AudioEngine engine;
        return engine;
    }

    // Lazy init of the mixing engine (this is where a platform backend gets
    // dlopen'd, or the null backend selected via ROXAL_AUDIO_BACKEND=null).
    // A failure is cached: audio stays unavailable for the process lifetime.
    bool ensureStarted(std::string& err)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return ensureStartedLocked(err);
    }

    bool available()
    {
        std::string err;
        return ensureStarted(err);
    }

    // Starts a mixed instance over an engine-owned copy of the PCM.
    // Returns 0 with err set on failure.
    uint64_t play(std::vector<float>&& pcm, uint32_t channels, uint32_t sampleRate,
                  float volume, float pan, bool loop, std::string& err)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureStartedLocked(err))
            return 0;
        reapLocked();

        auto inst = std::make_unique<Instance>();
        inst->pcm = std::move(pcm);
        ma_uint64 frameCount = inst->pcm.size() / channels;

        ma_audio_buffer_config bufCfg = ma_audio_buffer_config_init(
            ma_format_f32, channels, frameCount, inst->pcm.data(), nullptr);
        bufCfg.sampleRate = sampleRate;
        if (ma_audio_buffer_init(&bufCfg, &inst->buffer) != MA_SUCCESS) {
            err = "failed to create playback buffer";
            return 0;
        }
        inst->bufferInited = true;

        if (ma_sound_init_from_data_source(&engine_, &inst->buffer,
                                           MA_SOUND_FLAG_NO_SPATIALIZATION,
                                           nullptr, &inst->sound) != MA_SUCCESS) {
            uninitInstance(*inst);
            err = "failed to create playback instance";
            return 0;
        }
        inst->soundInited = true;

        ma_sound_set_volume(&inst->sound, volume);
        ma_sound_set_pan(&inst->sound, pan);
        ma_sound_set_looping(&inst->sound, loop ? MA_TRUE : MA_FALSE);
        if (ma_sound_start(&inst->sound) != MA_SUCCESS) {
            uninitInstance(*inst);
            err = "failed to start playback";
            return 0;
        }

        uint64_t id = nextId_++;
        instances_[id] = std::move(inst);
        return id;
    }

    void stop(uint64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(id);
        if (it == instances_.end())
            return;
        uninitInstance(*it->second);
        instances_.erase(it);
    }

    bool isPlaying(uint64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(id);
        return it != instances_.end() && it->second->soundInited &&
               ma_sound_is_playing(&it->second->sound);
    }

    void setVolume(uint64_t id, float volume)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(id);
        if (it != instances_.end() && it->second->soundInited)
            ma_sound_set_volume(&it->second->sound, volume);
    }

    // Blocking capture. Fills `out` (preallocated by caller) with interleaved
    // f32 frames. The audio callback writes only into `out` via an atomic
    // cursor — no locks and no GC state — so the caller may bracket the wait
    // in a GCSafeBlockScope.
    void capture(std::vector<float>& out, uint32_t channels, uint32_t sampleRate,
                 double seconds)
    {
        {
            // Device availability / null-backend context, shared with playback.
            std::string err;
            std::lock_guard<std::mutex> lock(mutex_);
            if (!ensureStartedLocked(err))
                throw std::runtime_error("media.record: " + err);
        }

        struct CaptureState {
            float* dst;
            uint64_t targetSamples;
            uint32_t channels;
            std::atomic<uint64_t> writtenSamples { 0 };
            std::mutex m;
            std::condition_variable cv;
        } state;
        state.dst = out.data();
        state.targetSamples = out.size();
        state.channels = channels;

        ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
        cfg.capture.format = ma_format_f32;
        cfg.capture.channels = channels;
        cfg.sampleRate = sampleRate;
        cfg.pUserData = &state;
        cfg.dataCallback = [](ma_device* dev, void*, const void* input, ma_uint32 frames) {
            auto* st = static_cast<CaptureState*>(dev->pUserData);
            uint64_t have = st->writtenSamples.load(std::memory_order_relaxed);
            uint64_t want = static_cast<uint64_t>(frames) * st->channels;
            uint64_t take = std::min(want, st->targetSamples - have);
            if (take > 0) {
                std::memcpy(st->dst + have, input, take * sizeof(float));
                st->writtenSamples.store(have + take, std::memory_order_release);
            }
            if (have + take >= st->targetSamples)
                st->cv.notify_one();
        };

        ma_device device;
        if (ma_device_init(contextInited_ ? &context_ : nullptr, &cfg, &device) != MA_SUCCESS)
            throw std::runtime_error("media.record: no capture device available");
        if (ma_device_start(&device) != MA_SUCCESS) {
            ma_device_uninit(&device);
            throw std::runtime_error("media.record: failed to start capture device");
        }

        bool complete;
        {
            // The GC cannot wake or poll this wait; park the thread for its
            // duration. Nothing inside touches GC state.
            SimpleMarkSweepGC::GCSafeBlockScope gcScope;
            std::unique_lock<std::mutex> lk(state.m);
            complete = state.cv.wait_for(lk,
                std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000) + 5000),
                [&] { return state.writtenSamples.load(std::memory_order_acquire) >= state.targetSamples; });
            lk.unlock();
            ma_device_uninit(&device);
        }
        if (!complete)
            throw std::runtime_error("media.record: capture stalled before completing");
    }

    // Stop everything and close the engine/device. Idempotent; called from
    // module unload (VM shutdown) and as a static-destructor backstop.
    void shutdown()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : instances_)
            uninitInstance(*entry.second);
        instances_.clear();
        if (engineInited_) {
            ma_engine_uninit(&engine_);
            engineInited_ = false;
        }
        if (contextInited_) {
            ma_context_uninit(&context_);
            contextInited_ = false;
        }
    }

private:
    struct Instance {
        std::vector<float> pcm;
        ma_audio_buffer buffer {};
        ma_sound sound {};
        bool bufferInited { false };
        bool soundInited { false };
    };

    AudioEngine() = default;
    ~AudioEngine() { shutdown(); }

    bool ensureStartedLocked(std::string& err)
    {
        if (engineInited_)
            return true;
        if (engineFailed_) {
            err = engineError_;
            return false;
        }

        ma_result result = MA_SUCCESS;
        const char* backendEnv = std::getenv("ROXAL_AUDIO_BACKEND");
        if (backendEnv != nullptr && *backendEnv != '\0') {
            if (std::string(backendEnv) != "null") {
                engineFailed_ = true;
                engineError_ = std::string("unknown ROXAL_AUDIO_BACKEND '") + backendEnv +
                               "' (supported: null)";
                err = engineError_;
                return false;
            }
            ma_backend backends[] = { ma_backend_null };
            result = ma_context_init(backends, 1, nullptr, &context_);
            if (result == MA_SUCCESS) {
                contextInited_ = true;
                ma_engine_config cfg = ma_engine_config_init();
                cfg.pContext = &context_;
                result = ma_engine_init(&cfg, &engine_);
            }
        } else {
            result = ma_engine_init(nullptr, &engine_);
        }

        if (result != MA_SUCCESS) {
            if (contextInited_) {
                ma_context_uninit(&context_);
                contextInited_ = false;
            }
            engineFailed_ = true;
            engineError_ = std::string("audio device unavailable (") +
                           ma_result_description(result) + ")";
            err = engineError_;
            return false;
        }
        engineInited_ = true;
        return true;
    }

    // Release instances whose sound has finished (or was stopped). The audio
    // thread no longer reads a non-playing sound, so freeing its PCM is safe;
    // ma_sound_uninit itself synchronizes detachment from the mix graph.
    void reapLocked()
    {
        for (auto it = instances_.begin(); it != instances_.end(); ) {
            if (it->second->soundInited && !ma_sound_is_playing(&it->second->sound)) {
                uninitInstance(*it->second);
                it = instances_.erase(it);
            } else {
                ++it;
            }
        }
    }

    static void uninitInstance(Instance& inst)
    {
        if (inst.soundInited) {
            ma_sound_uninit(&inst.sound);
            inst.soundInited = false;
        }
        if (inst.bufferInited) {
            ma_audio_buffer_uninit(&inst.buffer);
            inst.bufferInited = false;
        }
    }

    std::mutex mutex_;
    std::map<uint64_t, std::unique_ptr<Instance>> instances_;
    uint64_t nextId_ { 1 };
    ma_context context_ {};
    bool contextInited_ { false };
    ma_engine engine_ {};
    bool engineInited_ { false };
    bool engineFailed_ { false };
    std::string engineError_;
};

uint64_t playbackId(ArgsView args, const char* methodName)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument(std::string("Playback.") + methodName + " expects receiver");
    int64_t id = toType(ValueType::Int, asObjectInstance(args[0])->getProperty("_id"), false).asInt();
    return id > 0 ? static_cast<uint64_t>(id) : 0;
}

} // namespace

// ============================================================
// Registration / teardown
// ============================================================

void ModuleMedia::registerAudioBuiltins()
{
    linkMethod("Audio", "init",     [this](VM&, ArgsView a) { return audio_init_builtin(a); });
    linkMethod("Audio", "frames",   [this](VM&, ArgsView a) { return audio_frames_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Audio", "channels", [this](VM&, ArgsView a) { return audio_channels_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Audio", "duration", [this](VM&, ArgsView a) { return audio_duration_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Audio", "write",    [this](VM&, ArgsView a) { return audio_write_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Audio", "play",     [this](VM&, ArgsView a) { return audio_play_builtin(a); }, {}, 0, /*noMutateSelf=*/true);

    linkMethod("Playback", "stop",       [this](VM&, ArgsView a) { return playback_stop_builtin(a); });
    linkMethod("Playback", "playing",    [this](VM&, ArgsView a) { return playback_playing_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Playback", "set_volume", [this](VM&, ArgsView a) { return playback_set_volume_builtin(a); });

    link("audio_available", [this](VM&, ArgsView a) { return audio_available_builtin(a); });
    link("record",          [this](VM&, ArgsView a) { return record_builtin(a); });
}

void ModuleMedia::onModuleUnloading(VM& vm)
{
    (void)vm;
    AudioEngine::get().shutdown();
}

// ============================================================
// Audio.init(path, source, rate)
// ============================================================

Value ModuleMedia::audio_init_builtin(ArgsView args)
{
    ObjectInstance* inst = audioReceiver(args, "init");

    std::string path;
    if (args.size() >= 2 && isString(args[1]))
        path = toUTF8StdString(asStringObj(args[1])->s);
    bool hasSource = (args.size() >= 3 && isTensor(args[2]));
    int64_t rate = 0;
    if (args.size() >= 4 && !args[3].isNil())
        rate = toType(ValueType::Int, args[3], false).asInt();

    if (!path.empty() && hasSource)
        throw std::invalid_argument("Audio.init: provide either path or source, not both");

    if (hasSource) {
        ObjTensor* t = asTensor(args[2]);
        ClipShape shape = clipShape(t, "Audio.init");
        switch (t->dtype()) {
        case TensorDType::UInt8:
        case TensorDType::Int16:
        case TensorDType::Float32:
            break;
        default:
            throw std::invalid_argument("Audio.init: source dtype must be uint8, int16 or float32, got " +
                                        to_string(t->dtype()));
        }
        if (rate <= 0)
            throw std::invalid_argument("Audio.init: rate= (Hz) is required with a source tensor");
        (void)shape;
        inst->setProperty("data", args[2]);
        inst->setProperty("rate", Value::intVal(rate));
        return Value::nilVal();
    }

    if (path.empty())
        throw std::invalid_argument("Audio.init: provide a file path or a source tensor");

    std::string ext = toLowerStr(std::filesystem::path(path).extension().string());
    if (ext != ".wav" && ext != ".mp3" && ext != ".flac")
        throw std::invalid_argument("Audio.init: unsupported format '" + ext +
                                    "' (supported: .wav, .mp3, .flac)");
    if (!std::filesystem::exists(path))
        throw std::runtime_error("Audio.init: file not found: '" + path + "'");

    // Decode the whole file to interleaved f32 at its native channels/rate.
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_uint64 frameCount = 0;
    void* pcm = nullptr;
    ma_result result = ma_decode_file(path.c_str(), &cfg, &frameCount, &pcm);
    if (result != MA_SUCCESS)
        throw std::runtime_error("Audio.init: failed to decode '" + path + "' (" +
                                 ma_result_description(result) + ")");

    std::vector<int64_t> shape = { static_cast<int64_t>(frameCount),
                                   static_cast<int64_t>(cfg.channels) };
    auto tensor = newTensorObj(shape, TensorDType::Float32);
    fillTensorF32(tensor.get(), static_cast<const float*>(pcm),
                  static_cast<int64_t>(frameCount) * cfg.channels);
    ma_free(pcm, nullptr);

    inst->setProperty("data", Value::objVal(std::move(tensor)));
    inst->setProperty("rate", Value::intVal(static_cast<int64_t>(cfg.sampleRate)));
    return Value::nilVal();
}

// ============================================================
// Query methods
// ============================================================

Value ModuleMedia::audio_frames_builtin(ArgsView args)
{
    ObjectInstance* inst = audioReceiver(args, "frames");
    return Value::intVal(clipShape(audioTensor(inst, "frames"), "Audio.frames").frames);
}

Value ModuleMedia::audio_channels_builtin(ArgsView args)
{
    ObjectInstance* inst = audioReceiver(args, "channels");
    return Value::intVal(clipShape(audioTensor(inst, "channels"), "Audio.channels").channels);
}

Value ModuleMedia::audio_duration_builtin(ArgsView args)
{
    ObjectInstance* inst = audioReceiver(args, "duration");
    ClipShape shape = clipShape(audioTensor(inst, "duration"), "Audio.duration");
    return Value::realVal(static_cast<double>(shape.frames) /
                          static_cast<double>(audioRate(inst, "duration")));
}

// ============================================================
// Audio.write(path) — WAV, sample format matching the tensor dtype
// ============================================================

Value ModuleMedia::audio_write_builtin(ArgsView args)
{
    ObjectInstance* inst = audioReceiver(args, "write");
    if (args.size() < 2 || !isString(args[1]))
        throw std::invalid_argument("Audio.write: path must be a string");
    std::string path = toUTF8StdString(asStringObj(args[1])->s);

    std::string ext = toLowerStr(std::filesystem::path(path).extension().string());
    if (ext != ".wav")
        throw std::invalid_argument("Audio.write: unsupported format '" + ext +
                                    "' (supported: .wav)");

    ObjTensor* t = audioTensor(inst, "write");
    ClipShape shape = clipShape(t, "Audio.write");
    int64_t rate = audioRate(inst, "write");
    const int64_t n = t->numel();
    const bool typed = t->isOrtBacked();

    // Encode in the sample format matching the dtype (float64 narrows to f32).
    ma_format format;
    std::vector<uint8_t> raw;
    switch (t->dtype()) {
    case TensorDType::UInt8: {
        format = ma_format_u8;
        raw.resize(static_cast<size_t>(n));
        if (typed) {
            std::memcpy(raw.data(), t->rawData(), static_cast<size_t>(n));
        } else {
            for (int64_t i = 0; i < n; ++i)
                raw[i] = static_cast<uint8_t>(std::clamp(std::round(t->at(i)), 0.0, 255.0));
        }
        break;
    }
    case TensorDType::Int16: {
        format = ma_format_s16;
        raw.resize(static_cast<size_t>(n) * sizeof(int16_t));
        int16_t* dst = reinterpret_cast<int16_t*>(raw.data());
        if (typed) {
            std::memcpy(dst, t->rawData(), static_cast<size_t>(n) * sizeof(int16_t));
        } else {
            for (int64_t i = 0; i < n; ++i)
                dst[i] = static_cast<int16_t>(std::clamp(std::round(t->at(i)), -32768.0, 32767.0));
        }
        break;
    }
    case TensorDType::Float32:
    case TensorDType::Float64: {
        format = ma_format_f32;
        raw.resize(static_cast<size_t>(n) * sizeof(float));
        float* dst = reinterpret_cast<float*>(raw.data());
        if (typed && t->dtype() == TensorDType::Float32) {
            std::memcpy(dst, t->rawData(), static_cast<size_t>(n) * sizeof(float));
        } else {
            for (int64_t i = 0; i < n; ++i)
                dst[i] = static_cast<float>(t->at(i));
        }
        break;
    }
    default:
        throw std::invalid_argument("Audio.write: tensor dtype must be uint8, int16 or float32, got " +
                                    to_string(t->dtype()));
    }

    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, format,
                                                   static_cast<ma_uint32>(shape.channels),
                                                   static_cast<ma_uint32>(rate));
    ma_encoder encoder;
    if (ma_encoder_init_file(path.c_str(), &cfg, &encoder) != MA_SUCCESS)
        throw std::runtime_error("Audio.write: cannot write '" + path + "'");
    ma_uint64 written = 0;
    ma_result result = ma_encoder_write_pcm_frames(&encoder, raw.data(),
                                                   static_cast<ma_uint64>(shape.frames), &written);
    ma_encoder_uninit(&encoder);
    if (result != MA_SUCCESS || written != static_cast<ma_uint64>(shape.frames))
        throw std::runtime_error("Audio.write: failed writing '" + path + "'");

    return Value::boolVal(true);
}

// ============================================================
// Audio.play(volume, pan, loop) -> Playback
// ============================================================

Value ModuleMedia::audio_play_builtin(ArgsView args)
{
    ObjectInstance* inst = audioReceiver(args, "play");
    ObjTensor* t = audioTensor(inst, "play");
    ClipShape shape = clipShape(t, "Audio.play");
    int64_t rate = audioRate(inst, "play");
    if (shape.frames < 1)
        throw std::invalid_argument("Audio.play: clip is empty");

    double volume = 1.0, pan = 0.0;
    bool loop = false;
    if (args.size() >= 2 && !args[1].isNil())
        volume = toType(ValueType::Real, args[1], false).asReal();
    if (args.size() >= 3 && !args[2].isNil())
        pan = toType(ValueType::Real, args[2], false).asReal();
    if (args.size() >= 4 && !args[3].isNil())
        loop = args[3].asBool(false);
    volume = std::max(0.0, volume);
    pan = std::clamp(pan, -1.0, 1.0);

    std::vector<float> pcm = tensorToF32(t, "Audio.play");

    std::string err;
    uint64_t id = AudioEngine::get().play(std::move(pcm),
                                          static_cast<uint32_t>(shape.channels),
                                          static_cast<uint32_t>(rate),
                                          static_cast<float>(volume),
                                          static_cast<float>(pan), loop, err);
    if (id == 0)
        throw std::runtime_error("Audio.play: " + err);

    auto typeVal = asModuleType(moduleType())->vars.load(toUnicodeString("Playback"));
    if (!typeVal.has_value() || !isObjectType(typeVal.value()))
        throw std::runtime_error("Audio.play: Playback type not found in module");
    Value handle = Value::objVal(newObjectInstance(typeVal.value()));
    asObjectInstance(handle)->setProperty("_id", Value::intVal(static_cast<int64_t>(id)));
    return handle;
}

// ============================================================
// Playback methods
// ============================================================

Value ModuleMedia::playback_stop_builtin(ArgsView args)
{
    uint64_t id = playbackId(args, "stop");
    if (id != 0)
        AudioEngine::get().stop(id);
    return Value::nilVal();
}

Value ModuleMedia::playback_playing_builtin(ArgsView args)
{
    uint64_t id = playbackId(args, "playing");
    return Value::boolVal(id != 0 && AudioEngine::get().isPlaying(id));
}

Value ModuleMedia::playback_set_volume_builtin(ArgsView args)
{
    uint64_t id = playbackId(args, "set_volume");
    if (args.size() < 2)
        throw std::invalid_argument("Playback.set_volume expects a volume");
    double volume = std::max(0.0, toType(ValueType::Real, args[1], false).asReal());
    if (id != 0)
        AudioEngine::get().setVolume(id, static_cast<float>(volume));
    return Value::nilVal();
}

// ============================================================
// media.audio_available() / media.record(seconds, rate, channels)
// ============================================================

Value ModuleMedia::audio_available_builtin(ArgsView args)
{
    (void)args;
    return Value::boolVal(AudioEngine::get().available());
}

Value ModuleMedia::record_builtin(ArgsView args)
{
    // `duration` is a number of seconds or a time quantity (3s, 200ms, ...),
    // same contract as sys.wait's duration.
    double seconds = 0.0;
    if (args.size() >= 1 && !args[0].isNil()) {
        const Value& durationVal = args[0];
        if (durationVal.isNumber()) {
            seconds = durationVal.isReal() ? durationVal.asReal()
                                           : static_cast<double>(durationVal.asInt());
        } else if (auto q = sysTimeQuantitySeconds(durationVal, "media.record duration")) {
            seconds = *q;
        } else {
            throw std::invalid_argument(
                "media.record: duration must be seconds (a number) or a time quantity (e.g. 3s)");
        }
    }
    int64_t rate = 16000;
    if (args.size() >= 2 && !args[1].isNil())
        rate = toType(ValueType::Int, args[1], false).asInt();
    int64_t channels = 1;
    if (args.size() >= 3 && !args[2].isNil())
        channels = toType(ValueType::Int, args[2], false).asInt();

    if (seconds <= 0.0 || seconds > 3600.0)
        throw std::invalid_argument("media.record: seconds must be in (0, 3600]");
    if (rate < 1000 || rate > 384000)
        throw std::invalid_argument("media.record: rate must be 1000..384000 Hz");
    if (channels < 1 || channels > 32)
        throw std::invalid_argument("media.record: channels must be 1..32");

    int64_t frames = static_cast<int64_t>(std::llround(seconds * static_cast<double>(rate)));
    if (frames < 1)
        frames = 1;

    std::vector<float> samples(static_cast<size_t>(frames * channels));
    AudioEngine::get().capture(samples, static_cast<uint32_t>(channels),
                               static_cast<uint32_t>(rate), seconds);

    std::vector<int64_t> shape = { frames, channels };
    auto tensor = newTensorObj(shape, TensorDType::Float32);
    fillTensorF32(tensor.get(), samples.data(), frames * channels);

    auto typeVal = asModuleType(moduleType())->vars.load(toUnicodeString("Audio"));
    if (!typeVal.has_value() || !isObjectType(typeVal.value()))
        throw std::runtime_error("media.record: Audio type not found in module");
    Value clip = Value::objVal(newObjectInstance(typeVal.value()));
    asObjectInstance(clip)->setProperty("data", Value::objVal(std::move(tensor)));
    asObjectInstance(clip)->setProperty("rate", Value::intVal(rate));
    return clip;
}
