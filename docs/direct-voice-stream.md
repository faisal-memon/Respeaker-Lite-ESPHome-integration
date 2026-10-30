# Direct voice stream (first firmware milestone)

`direct_voice_stream` is an ESPHome external component for the temporary
ReSpeaker Lite to RAG receiver test. It sends the same protocol implemented by
the RAG API's `/agent/voice/stream` endpoint:

1. Connect to the configured `ws://` or `wss://` URL.
2. Send a JSON `start` frame for 16 kHz, mono, signed 16-bit PCM.
3. Send binary PCM frames while recording.
4. Send JSON `end` after the button releases.

It uses the ReSpeaker's existing `i2s_mics` resampled microphone. The C++
component queues audio before sending it, so a slow Wi-Fi connection cannot
block microphone capture. It currently sends only channel 0, the board's
speech-to-text channel.

The receiver is deliberately unauthenticated only for this LAN-only transport
test. Do not route it through a public proxy. Authentication is required before
the endpoint can accept production voice requests.

## Add it to a local test configuration

Use `config/respeaker-direct-stream.yaml` for the isolated test firmware. It
uses Improv over BLE or USB serial for Wi-Fi provisioning, and its push-to-talk
button sends microphone audio to the 3090 on the home LAN. Normal button
gestures and automatic wake-word listening are disabled in this test profile.

The endpoint acknowledges a valid recording after it has received all PCM
frames, then sends the in-memory WAV to the 3090's local `whisper.cpp` service.
The transcript is written to the `rag-voice` container log. The recording is
not saved, and the transcript is not sent to the agent or shown in chat.

To build and flash it over USB:

```sh
esphome run config/respeaker-direct-stream.yaml
```

The custom component uses the local checkout so changes to its C++ source are
included in the build. The ordinary factory and dashboard profiles are
unchanged.

The profile's LAN endpoint is intentionally configured for the 3090 host. For
this prototype, the API port is bound directly to the 3090's LAN address. That
makes the other API routes reachable from devices on the same LAN too. The
public agent proxy must continue to deny this WebSocket path.

For a minimal custom config, point ESPHome at the checked-out component while
developing it:

```yaml
external_components:
  - source:
      type: local
      path: ../esphome/components
    components: [direct_voice_stream]

direct_voice_stream:
  id: rag_voice_stream
  microphone: i2s_mics
  # Use the 3090's private LAN address; do not use the public proxy URL.
  url: ws://REPLACE_WITH_3090_LAN_ADDRESS:8080/agent/voice/stream
  on_error:
    - logger.log:
        format: "Voice stream failed: %s"
        args: ["message.c_str()"]

binary_sensor:
  - id: !extend user_button
    on_press:
      - direct_voice_stream.start: rag_voice_stream
    on_release:
      - direct_voice_stream.stop: rag_voice_stream
```

Use this only in a direct-audio profile. The standard satellite configuration
already assigns the button and microphone to Home Assistant's voice assistant.
