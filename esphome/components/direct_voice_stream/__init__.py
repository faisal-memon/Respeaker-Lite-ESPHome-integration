import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.automation import maybe_simple_id
from esphome.components import esp32, microphone, socket
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_URL
from esphome.core import CORE

CODEOWNERS = ["@fmemon"]
AUTO_LOAD = ["socket"]
DEPENDENCIES = ["microphone", "wifi"]
DOMAIN = "direct_voice_stream"

CONF_ON_ERROR = "on_error"
CONF_TRIGGER_ID = "trigger_id"

direct_voice_stream_ns = cg.esphome_ns.namespace("direct_voice_stream")
DirectVoiceStream = direct_voice_stream_ns.class_("DirectVoiceStream", cg.Component)
DirectVoiceStreamStartAction = direct_voice_stream_ns.class_(
    "DirectVoiceStreamStartAction", automation.Action
)
DirectVoiceStreamStopAction = direct_voice_stream_ns.class_(
    "DirectVoiceStreamStopAction", automation.Action
)
DirectVoiceStreamErrorTrigger = direct_voice_stream_ns.class_(
    "DirectVoiceStreamErrorTrigger", automation.Trigger.template(cg.std_string)
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(DirectVoiceStream),
            cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
            cv.Required(CONF_URL): cv.url,
            cv.Optional(CONF_ON_ERROR): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        DirectVoiceStreamErrorTrigger
                    ),
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    socket.consume_sockets(1, DOMAIN),
)


async def to_code(config):
    if not CORE.using_toolchain_esp_idf:
        raise cv.Invalid("direct_voice_stream requires the ESP-IDF framework")
    # ESP-IDF keeps the WebSocket client in the Component Registry rather than
    # its base framework. Pin it so every direct-stream build has the same API.
    esp32.add_idf_component(name="espressif/esp_websocket_client", ref="1.6.1")
    # The WebSocket component links ESP-TLS for both ws:// and wss:// URLs.
    esp32.request_tls()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    microphone_var = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(microphone_var))
    cg.add(var.set_url(config[CONF_URL]))

    for conf in config.get(CONF_ON_ERROR, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "message")], conf)


DIRECT_VOICE_STREAM_ACTION_SCHEMA = maybe_simple_id(
    {cv.Required(CONF_ID): cv.use_id(DirectVoiceStream)}
)


@automation.register_action(
    "direct_voice_stream.start",
    DirectVoiceStreamStartAction,
    DIRECT_VOICE_STREAM_ACTION_SCHEMA,
    synchronous=True,
)
async def direct_voice_stream_start_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    return var


@automation.register_action(
    "direct_voice_stream.stop",
    DirectVoiceStreamStopAction,
    DIRECT_VOICE_STREAM_ACTION_SCHEMA,
    synchronous=True,
)
async def direct_voice_stream_stop_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    return var
