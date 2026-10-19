#ifndef MICRO_WAKE_WORD_H
#define MICRO_WAKE_WORD_H

// Mochi build: wake word detection with microWakeWord (TFLite Micro streaming model + the micro speech
// feature frontend), ported from ESPHome's micro_wake_word component. Unlike the MultiNet-based
// CustomWakeWord it scores every 30 ms of audio continuously, so there are no detection windows to fall
// between, and the per-inference probability can be logged for tuning.

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <frontend.h>
#include <frontend_util.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_resource_variable.h>

#include "audio_codec.h"
#include "wake_word.h"

class MicroWakeWord : public WakeWord {
public:
    MicroWakeWord();
    ~MicroWakeWord();

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) override;
    void Feed(const std::vector<int16_t>& data) override;
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) override;
    void Start() override;
    void Stop() override;
    size_t GetFeedSize() override;
    void EncodeWakeWordData() override;
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override;
    const std::string& GetLastDetectedWakeWord() const override { return last_detected_wake_word_; }

private:
    static constexpr int kFeatureSize = 40;        // mel channels per feature slice
    static constexpr int kFeatureWindowMs = 30;
    static constexpr int kFeatureStepMs = 10;
    static constexpr int kIgnoreSlicesAfterReset = 100;   // ~1 s of slices before a detection may fire
    static constexpr size_t kVarArenaSize = 1024;

    bool LoadModel();
    bool GenerateFeatures(const int16_t* samples, size_t count, int8_t features[kFeatureSize], size_t* used);
    void RunInference(const int8_t features[kFeatureSize]);
    void ResetProbabilities();
    void StoreWakeWordData(const int16_t* data, size_t samples);

    AudioCodec* codec_ = nullptr;
    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    std::string wake_word_text_;
    std::string last_detected_wake_word_;
    std::atomic<bool> running_{false};
    std::mutex feed_mutex_;

    // Feature frontend
    FrontendConfig frontend_config_{};
    FrontendState frontend_state_{};
    bool frontend_ready_ = false;

    // Model
    const uint8_t* model_data_ = nullptr;
    tflite::MicroMutableOpResolver<20> op_resolver_;
    std::unique_ptr<tflite::MicroInterpreter> interpreter_;
    tflite::MicroResourceVariables* mrv_ = nullptr;
    tflite::MicroAllocator* var_allocator_ = nullptr;
    uint8_t* tensor_arena_ = nullptr;
    size_t tensor_arena_size_ = 0;
    uint8_t* var_arena_ = nullptr;
    int stride_ = 1;
    int stride_step_ = 0;

    // Detection
    uint8_t cutoff_ = 0;                       // quantised 0-255
    std::vector<uint8_t> window_;              // last N probabilities
    size_t window_index_ = 0;
    int ignore_slices_ = -kIgnoreSlicesAfterReset;

    // Tuning log: one line per "peak" (probability rose above a floor and fell back)
    bool peak_open_ = false;
    uint8_t peak_max_prob_ = 0;
    uint8_t peak_max_avg_ = 0;
    int peak_quiet_slices_ = 0;
    bool peak_in_cooloff_ = false;

    // Wake word audio for the server (same scheme as CustomWakeWord)
    std::vector<int16_t> mono_buffer_;
    // Last ~2 s of mono audio, as a ring buffer in PSRAM (small per-chunk vectors would land in internal SRAM).
    static constexpr size_t kPcmRingSamples = 16000 * 2;
    int16_t* pcm_ring_ = nullptr;
    size_t pcm_write_ = 0;
    size_t pcm_count_ = 0;
    std::deque<std::vector<uint8_t>> wake_word_opus_;
    std::mutex wake_word_mutex_;
    std::condition_variable wake_word_cv_;
    TaskHandle_t wake_word_encode_task_ = nullptr;
    StaticTask_t* wake_word_encode_task_buffer_ = nullptr;
    StackType_t* wake_word_encode_task_stack_ = nullptr;
};

#endif
