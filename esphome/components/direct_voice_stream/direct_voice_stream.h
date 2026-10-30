#pragma once

#ifdef USE_ESP32

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_event.h"

typedef struct esp_websocket_client *esp_websocket_client_handle_t;

namespace esphome::direct_voice_stream {

class DirectVoiceStream : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_microphone(microphone::Microphone *microphone) { this->microphone_ = microphone; }
  void set_url(const std::string &url) { this->url_ = url; }
  void start();
  void stop();
  void add_on_error_callback(std::function<void(const std::string &)> &&callback) {
    this->error_callback_.add(std::move(callback));
  }

 protected:
  static constexpr uint32_t SAMPLE_RATE = 16000;
  static constexpr uint8_t CHANNELS = 1;
  static constexpr uint8_t BYTES_PER_SAMPLE = 2;
  static constexpr size_t PCM_CHUNK_BYTES = 512;
  static constexpr size_t PCM_QUEUE_LENGTH = 32;
  static constexpr uint32_t MAX_AUDIO_BYTES = SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE * 30;

  struct PcmChunk {
    uint16_t size;
    std::array<uint8_t, PCM_CHUNK_BYTES> data;
  };

  static void websocket_event_(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
  void handle_websocket_event_(int32_t event_id, void *event_data);
  void connect_();
  void start_capture_();
  void queue_audio_(const std::vector<uint8_t> &data);
  void drain_audio_();
  void finish_stream_();
  void fail_(const std::string &message);
  bool websocket_connected_() const;

  microphone::Microphone *microphone_{nullptr};
  std::unique_ptr<microphone::MicrophoneSource> microphone_source_;
  std::string url_;
  esp_websocket_client_handle_t websocket_{nullptr};
  QueueHandle_t pcm_queue_{nullptr};
  CallbackManager<void(const std::string &)> error_callback_{};

  // ESPHome calls loop() and ESP-IDF delivers microphone/WebSocket callbacks
  // from different tasks. Queue payloads are FreeRTOS-safe; these flags and
  // counters make the surrounding state safe as well.
  std::atomic_bool start_requested_{false};
  std::atomic_bool capture_active_{false};
  std::atomic_bool end_requested_{false};
  std::atomic_bool protocol_started_{false};
  std::atomic_bool awaiting_ready_{false};
  std::atomic_bool awaiting_completion_{false};
  std::atomic_bool disconnected_{false};
  std::atomic_bool limit_reached_{false};
  std::atomic_bool closing_expected_{false};
  std::atomic_uint32_t queued_bytes_{0};
  std::atomic_uint32_t captured_bytes_{0};
  std::atomic_uint32_t dropped_bytes_{0};
};

template<typename... Ts> class DirectVoiceStreamStartAction : public Action<Ts...> {
 public:
  explicit DirectVoiceStreamStartAction(DirectVoiceStream *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->start(); }

 protected:
  DirectVoiceStream *parent_;
};

template<typename... Ts> class DirectVoiceStreamStopAction : public Action<Ts...> {
 public:
  explicit DirectVoiceStreamStopAction(DirectVoiceStream *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->stop(); }

 protected:
  DirectVoiceStream *parent_;
};

class DirectVoiceStreamErrorTrigger : public Trigger<std::string> {
 public:
  explicit DirectVoiceStreamErrorTrigger(DirectVoiceStream *parent) {
    parent->add_on_error_callback([this](const std::string &message) { this->trigger(message); });
  }
};

}  // namespace esphome::direct_voice_stream

#endif  // USE_ESP32
