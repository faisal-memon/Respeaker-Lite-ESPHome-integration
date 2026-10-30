#include "direct_voice_stream.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cstring>

#include "esp_event.h"
#include "esp_websocket_client.h"

#include "esphome/core/log.h"

namespace esphome::direct_voice_stream {

static const char *const TAG = "direct_voice_stream";

void DirectVoiceStream::setup() {
  if (this->microphone_ == nullptr) {
    this->mark_failed();
    this->fail_("A microphone is required");
    return;
  }

  this->pcm_queue_ = xQueueCreate(PCM_QUEUE_LENGTH, sizeof(PcmChunk));
  if (this->pcm_queue_ == nullptr) {
    this->mark_failed();
    this->fail_("Could not allocate the PCM queue");
    return;
  }

  // The ReSpeaker package exposes i2s_mics as 16 kHz stereo. Channel 0 is
  // the speech-to-text channel; MicrophoneSource converts it to 16-bit mono.
  this->microphone_source_ = make_unique<microphone::MicrophoneSource>(this->microphone_, 16, 1, false);
  this->microphone_source_->add_channel(0);
  this->microphone_source_->add_data_callback(
      [this](const std::vector<uint8_t> &data) { this->queue_audio_(data); });
}

void DirectVoiceStream::dump_config() {
  ESP_LOGCONFIG(TAG, "Direct voice stream:");
  ESP_LOGCONFIG(TAG, "  URL: %s", this->url_.c_str());
  ESP_LOGCONFIG(TAG, "  Audio: %lu Hz, %u channel, %u-bit PCM", static_cast<unsigned long>(SAMPLE_RATE), CHANNELS,
                BYTES_PER_SAMPLE * 8);
}

void DirectVoiceStream::start() {
  if (this->is_failed())
    return;
  if (this->capture_active_ || this->start_requested_) {
    ESP_LOGW(TAG, "A direct voice stream is already active");
    return;
  }

  this->start_requested_ = true;
  this->end_requested_ = false;
  this->protocol_started_ = false;
  this->awaiting_ready_ = false;
  this->awaiting_completion_ = false;
  this->abort_requested_ = false;
  this->disconnected_ = false;
  this->limit_reached_ = false;
  this->closing_expected_ = false;
  this->queued_bytes_ = 0;
  this->captured_bytes_ = 0;
  this->dropped_bytes_ = 0;
  xQueueReset(this->pcm_queue_);
  this->connect_();
}

void DirectVoiceStream::stop() {
  if (!this->capture_active_) {
    // The button can be released while the WebSocket handshake is still in
    // progress. Remember that release so start_capture_() doesn't start a
    // recording after the user has already let go of the button.
    if (this->start_requested_) {
      ESP_LOGD(TAG, "Button released before the voice receiver was ready");
      this->end_requested_ = true;
    }
    return;
  }
  this->microphone_source_->stop();
  this->capture_active_ = false;
  this->end_requested_ = true;
}

void DirectVoiceStream::connect_() {
  esp_websocket_client_config_t config = {};
  config.uri = this->url_.c_str();
  this->websocket_ = esp_websocket_client_init(&config);
  if (this->websocket_ == nullptr) {
    this->fail_("Could not initialize the WebSocket client");
    this->start_requested_ = false;
    return;
  }
  esp_websocket_register_events(this->websocket_, WEBSOCKET_EVENT_ANY, websocket_event_, this);
  if (esp_websocket_client_start(this->websocket_) != ESP_OK) {
    this->fail_("Could not start the WebSocket client");
    esp_websocket_client_destroy(this->websocket_);
    this->websocket_ = nullptr;
    this->start_requested_ = false;
  }
}

void DirectVoiceStream::loop() {
  if (this->disconnected_.exchange(false)) {
    if (this->websocket_ != nullptr) {
      esp_websocket_client_destroy(this->websocket_);
      this->websocket_ = nullptr;
    }
    this->start_requested_ = false;
    this->end_requested_ = false;
    this->protocol_started_ = false;
    this->awaiting_ready_ = false;
    this->awaiting_completion_ = false;
    this->abort_requested_ = false;
    this->closing_expected_ = false;
    return;
  }
  if (this->abort_requested_.exchange(false) && this->websocket_ != nullptr) {
    this->closing_expected_ = true;
    esp_websocket_client_stop(this->websocket_);
    return;
  }
  if (!this->websocket_connected_())
    return;
  if (this->limit_reached_ && this->capture_active_)
    this->stop();
  this->drain_audio_();
  // Do not send the end frame until the receiver has replied "ready" and
  // capture has actually started. If the button is released during the
  // handshake, the ready handler cancels the empty session instead.
  if (this->protocol_started_ && this->end_requested_ && uxQueueMessagesWaiting(this->pcm_queue_) == 0) {
    this->finish_stream_();
  }
}

void DirectVoiceStream::websocket_event_(void *handler_args, esp_event_base_t, int32_t event_id, void *event_data) {
  static_cast<DirectVoiceStream *>(handler_args)->handle_websocket_event_(event_id, event_data);
}

void DirectVoiceStream::handle_websocket_event_(int32_t event_id, void *event_data) {
  if (event_id == WEBSOCKET_EVENT_CONNECTED) {
    ESP_LOGI(TAG, "Connected to the voice receiver");
    const char *start = "{\"type\":\"start\",\"version\":1,\"encoding\":\"pcm_s16le\",\"sample_rate_hz\":16000,\"channels\":1}";
    if (esp_websocket_client_send_text(this->websocket_, start, strlen(start), pdMS_TO_TICKS(1000)) < 0) {
      this->fail_("Could not send the stream start message");
      return;
    }
    this->awaiting_ready_ = true;
    return;
  }
  if (event_id == WEBSOCKET_EVENT_DATA) {
    auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
    if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
      std::string message(data->data_ptr, data->data_len);
      if (this->awaiting_ready_ && message.find("\"type\":\"ready\"") != std::string::npos) {
        this->awaiting_ready_ = false;
        if (this->end_requested_) {
          // The user released the button before the receiver completed its
          // handshake. Close this empty session; disconnected_ lets loop()
          // reclaim the client and reset all per-stream state.
          ESP_LOGI(TAG, "Button was released before recording began; cancelling voice stream");
          this->abort_requested_ = true;
          return;
        }
        this->start_capture_();
      } else if (message.find("\"type\":\"error\"") != std::string::npos) {
        this->fail_("Voice receiver rejected the stream");
        this->abort_requested_ = true;
      } else if (this->awaiting_completion_ && message.find("\"type\":\"complete\"") != std::string::npos) {
        this->awaiting_completion_ = false;
        this->start_requested_ = false;
        this->closing_expected_ = true;
        ESP_LOGI(TAG, "Voice receiver accepted the completed audio stream");
      }
    }
    return;
  }
  if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
    if (!this->closing_expected_ && (this->capture_active_ || this->start_requested_))
      this->fail_("Voice receiver disconnected before the stream completed");
    this->disconnected_ = true;
    return;
  }
  if (event_id == WEBSOCKET_EVENT_ERROR) {
    this->fail_("WebSocket transport error");
    this->abort_requested_ = true;
  }
}

void DirectVoiceStream::start_capture_() {
  this->protocol_started_ = true;
  this->microphone_source_->start();
  this->capture_active_ = true;
  ESP_LOGI(TAG, "Recording direct voice stream");
}

void DirectVoiceStream::queue_audio_(const std::vector<uint8_t> &data) {
  if (!this->capture_active_ || !this->protocol_started_)
    return;
  for (size_t offset = 0; offset < data.size(); offset += PCM_CHUNK_BYTES) {
    PcmChunk chunk{};
    const auto remaining = MAX_AUDIO_BYTES - this->captured_bytes_;
    if (remaining == 0) {
      ESP_LOGW(TAG, "Reached the 30-second stream limit");
      this->limit_reached_ = true;
      return;
    }
    chunk.size = std::min({PCM_CHUNK_BYTES, data.size() - offset, static_cast<size_t>(remaining)});
    memcpy(chunk.data.data(), data.data() + offset, chunk.size);
    this->captured_bytes_ += chunk.size;
    if (xQueueSend(this->pcm_queue_, &chunk, 0) != pdTRUE) {
      this->dropped_bytes_ += chunk.size;
    } else {
      this->queued_bytes_ += chunk.size;
    }
  }
}

void DirectVoiceStream::drain_audio_() {
  PcmChunk chunk{};
  while (xQueueReceive(this->pcm_queue_, &chunk, 0) == pdTRUE) {
    this->queued_bytes_ -= chunk.size;
    if (esp_websocket_client_send_bin(this->websocket_, reinterpret_cast<const char *>(chunk.data.data()), chunk.size,
                                      pdMS_TO_TICKS(100)) < 0) {
      this->fail_("Could not send microphone audio");
      this->stop();
      return;
    }
  }
}

void DirectVoiceStream::finish_stream_() {
  const char *end = "{\"type\":\"end\"}";
  if (esp_websocket_client_send_text(this->websocket_, end, strlen(end), pdMS_TO_TICKS(1000)) < 0) {
    this->fail_("Could not send the stream end message");
    this->closing_expected_ = true;
    esp_websocket_client_stop(this->websocket_);
    return;
  }
  ESP_LOGI(TAG, "Finished voice stream; dropped %lu bytes", static_cast<unsigned long>(this->dropped_bytes_.load()));
  this->end_requested_ = false;
  this->awaiting_completion_ = true;
}

bool DirectVoiceStream::websocket_connected_() const {
  return this->websocket_ != nullptr && esp_websocket_client_is_connected(this->websocket_);
}

void DirectVoiceStream::fail_(const std::string &message) {
  ESP_LOGE(TAG, "%s", message.c_str());
  this->error_callback_.call(message);
}

}  // namespace esphome::direct_voice_stream

#endif  // USE_ESP32
