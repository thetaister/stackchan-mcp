#include "micro_wake_word.h"
#include "audio_service.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstring>

#include <tensorflow/lite/micro/micro_allocator.h>
#include <tensorflow/lite/schema/schema_generated.h>

#define TAG "MicroWakeWord"

// The model is embedded with target_add_binary_data() under a fixed name (see main/CMakeLists.txt).
extern const uint8_t mww_model_start[] asm("_binary_mww_model_tflite_start");

namespace {

// Probability floor for the tuning log: a peak is logged once it rises above this (~0.30).
constexpr uint8_t kLogFloor = 77;
// Slices below the floor before a peak counts as finished.
constexpr int kPeakEndSlices = 10;

// PSRAM first (ESPHome's default too): internal SRAM is scarce on Mochi (Wi-Fi, LVGL, camera), and the
// v11 engine test showed free internal SRAM dropping to ~6 KB with the arena there.
uint8_t* AllocPreferPsram(size_t size) {
    auto* p = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (p == nullptr) {
        p = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return p;
}

}  // namespace

MicroWakeWord::MicroWakeWord() {}

MicroWakeWord::~MicroWakeWord() {
    interpreter_.reset();
    if (tensor_arena_ != nullptr) heap_caps_free(tensor_arena_);
    if (var_arena_ != nullptr) heap_caps_free(var_arena_);
    if (frontend_ready_) FrontendFreeStateContents(&frontend_state_);
    if (wake_word_encode_task_stack_ != nullptr) heap_caps_free(wake_word_encode_task_stack_);
    if (wake_word_encode_task_buffer_ != nullptr) heap_caps_free(wake_word_encode_task_buffer_);
    if (pcm_ring_ != nullptr) heap_caps_free(pcm_ring_);
}

bool MicroWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* /*models_list*/) {
    codec_ = codec;
    wake_word_text_ = CONFIG_MICRO_WAKE_WORD_DISPLAY;
    cutoff_ = static_cast<uint8_t>(std::min(255, CONFIG_MICRO_WAKE_WORD_CUTOFF * 255 / 100));
    window_.assign(std::max(1, CONFIG_MICRO_WAKE_WORD_WINDOW), 0);

    // Feature frontend: identical settings to ESPHome's preprocessor (the models are trained on these).
    frontend_config_.window.size_ms = kFeatureWindowMs;
    frontend_config_.window.step_size_ms = kFeatureStepMs;
    frontend_config_.filterbank.num_channels = kFeatureSize;
    frontend_config_.filterbank.lower_band_limit = 125.0f;
    frontend_config_.filterbank.upper_band_limit = 7500.0f;
    frontend_config_.noise_reduction.smoothing_bits = 10;
    frontend_config_.noise_reduction.even_smoothing = 0.025f;
    frontend_config_.noise_reduction.odd_smoothing = 0.06f;
    frontend_config_.noise_reduction.min_signal_remaining = 0.05f;
    frontend_config_.pcan_gain_control.enable_pcan = 1;
    frontend_config_.pcan_gain_control.strength = 0.95f;
    frontend_config_.pcan_gain_control.offset = 80.0f;
    frontend_config_.pcan_gain_control.gain_bits = 21;
    frontend_config_.log_scale.enable_log = 1;
    frontend_config_.log_scale.scale_shift = 6;
    if (!FrontendPopulateState(&frontend_config_, &frontend_state_, 16000)) {
        ESP_LOGE(TAG, "Failed to set up the feature frontend");
        return false;
    }
    frontend_ready_ = true;

    if (!LoadModel()) {
        return false;
    }
    pcm_ring_ = static_cast<int16_t*>(heap_caps_malloc(kPcmRingSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (pcm_ring_ == nullptr) {
        ESP_LOGW(TAG, "No PSRAM for the wake word audio buffer; wake word audio upload disabled");
    }
    ESP_LOGI(TAG, "Ready: \"%s\", cutoff %.2f, window %d, arena %u bytes (%s), stride %d",
             wake_word_text_.c_str(), cutoff_ / 255.0f, (int)window_.size(), (unsigned)tensor_arena_size_,
             esp_ptr_internal(tensor_arena_) ? "internal" : "PSRAM",
             stride_);
    return true;
}

bool MicroWakeWord::LoadModel() {
    model_data_ = mww_model_start;
    const tflite::Model* model = tflite::GetModel(model_data_);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema version %lu is not supported", (unsigned long)model->version());
        return false;
    }

    // Ops used by microWakeWord streaming models (same list as ESPHome).
    op_resolver_.AddCallOnce();
    op_resolver_.AddVarHandle();
    op_resolver_.AddReshape();
    op_resolver_.AddReadVariable();
    op_resolver_.AddStridedSlice();
    op_resolver_.AddConcatenation();
    op_resolver_.AddAssignVariable();
    op_resolver_.AddConv2D();
    op_resolver_.AddMul();
    op_resolver_.AddAdd();
    op_resolver_.AddMean();
    op_resolver_.AddFullyConnected();
    op_resolver_.AddLogistic();
    op_resolver_.AddQuantize();
    op_resolver_.AddDepthwiseConv2D();
    op_resolver_.AddAveragePool2D();
    op_resolver_.AddMaxPool2D();
    op_resolver_.AddPad();
    op_resolver_.AddPack();
    op_resolver_.AddSplitV();

    var_arena_ = AllocPreferPsram(kVarArenaSize);
    if (var_arena_ == nullptr) {
        ESP_LOGE(TAG, "Could not allocate the variable arena");
        return false;
    }

    // The manifest arena size varies a little with library versions, so try it, then 1.5x and 2x.
    const size_t base = (CONFIG_MICRO_WAKE_WORD_ARENA_SIZE + 15) & ~size_t(15);
    const size_t attempts[] = {base, (base * 3 / 2 + 15) & ~size_t(15), (base * 2 + 15) & ~size_t(15)};
    for (size_t size : attempts) {
        tensor_arena_ = AllocPreferPsram(size);
        if (tensor_arena_ == nullptr) continue;
        var_allocator_ = tflite::MicroAllocator::Create(var_arena_, kVarArenaSize);
        mrv_ = tflite::MicroResourceVariables::Create(var_allocator_, 20);
        interpreter_ = std::make_unique<tflite::MicroInterpreter>(model, op_resolver_, tensor_arena_, size, mrv_);
        if (interpreter_->AllocateTensors() == kTfLiteOk) {
            tensor_arena_size_ = size;
            break;
        }
        interpreter_.reset();
        heap_caps_free(tensor_arena_);
        tensor_arena_ = nullptr;
    }
    if (interpreter_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate tensors (arena up to %u bytes)", (unsigned)attempts[2]);
        return false;
    }

    TfLiteTensor* input = interpreter_->input(0);
    if (input->dims->size != 3 || input->dims->data[0] != 1 || input->dims->data[2] != kFeatureSize ||
        input->type != kTfLiteInt8) {
        ESP_LOGE(TAG, "Unexpected model input shape/type");
        return false;
    }
    TfLiteTensor* output = interpreter_->output(0);
    if (output->dims->size != 2 || output->dims->data[0] != 1 || output->dims->data[1] != 1 ||
        output->type != kTfLiteUInt8) {
        ESP_LOGE(TAG, "Unexpected model output shape/type");
        return false;
    }
    stride_ = input->dims->data[1];
    return true;
}

void MicroWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void MicroWakeWord::ResetProbabilities() {
    std::fill(window_.begin(), window_.end(), 0);
    ignore_slices_ = -kIgnoreSlicesAfterReset;
    peak_open_ = false;
    peak_max_prob_ = 0;
    peak_max_avg_ = 0;
    peak_quiet_slices_ = 0;
    peak_in_cooloff_ = false;
}

void MicroWakeWord::Start() {
    std::lock_guard<std::mutex> lock(feed_mutex_);
    if (frontend_ready_) FrontendReset(&frontend_state_);
    mono_buffer_.clear();
    stride_step_ = 0;
    ResetProbabilities();
    running_ = true;
}

void MicroWakeWord::Stop() {
    running_ = false;
    std::lock_guard<std::mutex> lock(feed_mutex_);
    mono_buffer_.clear();
}

size_t MicroWakeWord::GetFeedSize() {
    return 16000 * kFeatureStepMs / 1000;   // 160 samples (10 ms)
}

bool MicroWakeWord::GenerateFeatures(const int16_t* samples, size_t count, int8_t features[kFeatureSize],
                                     size_t* used) {
    *used = 0;
    FrontendOutput out = FrontendProcessSamples(&frontend_state_, samples, count, used);
    if (out.size == 0) return false;
    for (size_t i = 0; i < out.size && i < (size_t)kFeatureSize; ++i) {
        // Same int8 scaling as ESPHome / the TFLite audio frontend: (feature * 256) / 666 - 128.
        int32_t v = ((static_cast<int32_t>(out.values[i]) * 256) + 333) / 666 - 128;
        features[i] = static_cast<int8_t>(std::clamp<int32_t>(v, -128, 127));
    }
    return true;
}

void MicroWakeWord::RunInference(const int8_t features[kFeatureSize]) {
    TfLiteTensor* input = interpreter_->input(0);
    stride_step_ %= stride_;
    std::memcpy(input->data.int8 + kFeatureSize * stride_step_, features, kFeatureSize);
    // Cool-off after start or a detection counts 10 ms feature slices (as ESPHome does), so it lasts ~1 s.
    if (window_[window_index_] < cutoff_) {
        ignore_slices_ = std::min(ignore_slices_ + 1, 0);
    }
    if (++stride_step_ < stride_) {
        return;   // the model consumes `stride_` slices per invocation
    }
    if (interpreter_->Invoke() != kTfLiteOk) {
        ESP_LOGW(TAG, "Invoke failed");
        return;
    }
    const uint8_t prob = interpreter_->output(0)->data.uint8[0];
    window_index_ = (window_index_ + 1) % window_.size();
    window_[window_index_] = prob;

    uint32_t sum = 0;
    for (uint8_t p : window_) sum += p;
    const uint8_t avg = static_cast<uint8_t>(sum / window_.size());

    // Tuning log: one line per peak, so near-misses show up in the serial log.
    if (prob >= kLogFloor) {
        peak_open_ = true;
        peak_quiet_slices_ = 0;
        peak_max_prob_ = std::max(peak_max_prob_, prob);
        peak_max_avg_ = std::max(peak_max_avg_, avg);
        if (ignore_slices_ < 0) peak_in_cooloff_ = true;
    } else if (peak_open_ && ++peak_quiet_slices_ >= kPeakEndSlices) {
        ESP_LOGI(TAG, "peak: max %.2f, window avg %.2f (cutoff %.2f) - not detected%s", peak_max_prob_ / 255.0f,
                 peak_max_avg_ / 255.0f, cutoff_ / 255.0f, peak_in_cooloff_ ? " (cool-off after start)" : "");
        peak_in_cooloff_ = false;
        peak_open_ = false;
        peak_max_prob_ = 0;
        peak_max_avg_ = 0;
    }

    if (ignore_slices_ < 0) return;
    if (sum > static_cast<uint32_t>(cutoff_) * window_.size()) {
        ESP_LOGI(TAG, "Wake word detected: \"%s\" (prob %.2f, window avg %.2f, cutoff %.2f)",
                 wake_word_text_.c_str(), prob / 255.0f, avg / 255.0f, cutoff_ / 255.0f);
        ResetProbabilities();
        last_detected_wake_word_ = wake_word_text_;
        running_ = false;
        if (wake_word_detected_callback_) {
            wake_word_detected_callback_(last_detected_wake_word_);
        }
    }
}

void MicroWakeWord::Feed(const std::vector<int16_t>& data) {
    if (!running_ || interpreter_ == nullptr) return;
    std::lock_guard<std::mutex> lock(feed_mutex_);
    if (!running_) return;

    const size_t before = mono_buffer_.size();
    if (codec_->input_channels() == 2) {
        for (size_t i = 0; i < data.size(); i += 2) mono_buffer_.push_back(data[i]);
    } else {
        mono_buffer_.insert(mono_buffer_.end(), data.begin(), data.end());
    }
    StoreWakeWordData(mono_buffer_.data() + before, mono_buffer_.size() - before);

    size_t offset = 0;
    int8_t features[kFeatureSize];
    while (running_ && offset < mono_buffer_.size()) {
        size_t used = 0;
        bool have = GenerateFeatures(mono_buffer_.data() + offset, mono_buffer_.size() - offset, features, &used);
        if (used == 0) break;
        offset += used;
        if (have) RunInference(features);
    }
    mono_buffer_.erase(mono_buffer_.begin(), mono_buffer_.begin() + std::min(offset, mono_buffer_.size()));
}

void MicroWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {
    if (pcm_ring_ == nullptr) return;
    for (size_t i = 0; i < samples; ++i) {
        pcm_ring_[pcm_write_] = data[i];
        pcm_write_ = (pcm_write_ + 1) % kPcmRingSamples;
    }
    pcm_count_ = std::min(kPcmRingSamples, pcm_count_ + samples);
}

void MicroWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 7;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (MicroWakeWord*)arg;
        {
            auto start_time = esp_timer_get_time();
            esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
            void* encoder_handle = nullptr;
            auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
            if (encoder_handle == nullptr) {
                ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
                {
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    this_->wake_word_opus_.push_back(std::vector<uint8_t>());
                    this_->wake_word_cv_.notify_all();
                }
                vTaskDelete(NULL);   // never returns; the lock above is already released
                return;
            }
            int frame_size = 0;
            int outbuf_size = 0;
            esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
            frame_size = frame_size / sizeof(int16_t);
            int packets = 0;
            std::vector<int16_t> in_buffer;
            esp_audio_enc_in_frame_t in = {};
            esp_audio_enc_out_frame_t out = {};
            // Oldest-first copy out of the PSRAM ring (one big vector, so it is allocated in PSRAM too).
            if (this_->pcm_ring_ != nullptr && this_->pcm_count_ > 0) {
                const size_t n = this_->pcm_count_;
                const size_t start = (this_->pcm_write_ + kPcmRingSamples - n) % kPcmRingSamples;
                in_buffer.resize(n);
                for (size_t i = 0; i < n; ++i) in_buffer[i] = this_->pcm_ring_[(start + i) % kPcmRingSamples];
            }
            size_t pos = 0;
            {
                while (in_buffer.size() - pos >= (size_t)frame_size) {
                    std::vector<uint8_t> opus_buf(outbuf_size);
                    in.buffer = (uint8_t*)(in_buffer.data() + pos);
                    in.len = (uint32_t)(frame_size * sizeof(int16_t));
                    out.buffer = opus_buf.data();
                    out.len = outbuf_size;
                    out.encoded_bytes = 0;
                    ret = esp_opus_enc_process(encoder_handle, &in, &out);
                    if (ret == ESP_AUDIO_ERR_OK) {
                        std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                        this_->wake_word_opus_.emplace_back(opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                        this_->wake_word_cv_.notify_all();
                        packets++;
                    } else {
                        ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                    }
                    pos += frame_size;
                }
            }
            this_->pcm_count_ = 0;
            esp_opus_enc_close(encoder_handle);
            auto end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets,
                     (long)((end_time - start_time) / 1000));

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool MicroWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() { return !wake_word_opus_.empty(); });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
