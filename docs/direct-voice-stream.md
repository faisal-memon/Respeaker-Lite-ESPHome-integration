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

Point ESPHome at the checked-out component while developing it:

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
The next change will add that profile without changing the existing one.
