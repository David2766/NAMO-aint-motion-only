import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ld2410, ld2450, socket, uart, web_server_base
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32", "ld2410", "ld2450", "mdns", "uart", "web_server_base"]
AUTO_LOAD = ["json", "sha256", "socket"]

CONF_WEB_SERVER_BASE_ID = "web_server_base_id"
CONF_LD2450_ID = "ld2450_id"
CONF_LD2410_ID = "ld2410_id"
CONF_LD2410_UART_ID = "ld2410_uart_id"
CONF_SHADOW_TRANSPORT_SMOKE = "shadow_transport_smoke"
CONF_PRODUCTION_FUSION = "production_fusion"
CONF_SIGNED_OTA = "signed_ota"

radar_api_server_ns = cg.esphome_ns.namespace("radar_api_server")
RadarApiServer = radar_api_server_ns.class_("RadarApiServer", cg.Component)


def _consume_transport_sockets(config):
    socket.consume_sockets(2, "radar_api_server", socket.SocketType.UDP)(config)
    if config[CONF_SHADOW_TRANSPORT_SMOKE] or config[CONF_PRODUCTION_FUSION]:
        socket.consume_sockets(
            1, "radar_api_server.observation_transport", socket.SocketType.UDP
        )(config)
    if config[CONF_SHADOW_TRANSPORT_SMOKE] and config[CONF_PRODUCTION_FUSION]:
        raise cv.Invalid(
            "shadow_transport_smoke and production_fusion are mutually exclusive"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RadarApiServer),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
                web_server_base.WebServerBase
            ),
            cv.GenerateID(CONF_LD2450_ID): cv.use_id(ld2450.LD2450Component),
            cv.GenerateID(CONF_LD2410_ID): cv.use_id(ld2410.LD2410Component),
            cv.GenerateID(CONF_LD2410_UART_ID): cv.use_id(uart.UARTComponent),
            cv.Optional(CONF_SHADOW_TRANSPORT_SMOKE, default=False): cv.boolean,
            cv.Optional(CONF_PRODUCTION_FUSION, default=False): cv.boolean,
            cv.Optional(CONF_SIGNED_OTA, default=False): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _consume_transport_sockets,
)


async def to_code(config):
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_HKDF_C", True)
    cg.add_build_flag('-DMBEDTLS_CONFIG_FILE=\\"mbedtls/esp_config.h\\"')
    cg.add_build_flag("-Wl,--wrap=httpd_start")
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], base)
    await cg.register_component(var, config)
    radar = await cg.get_variable(config[CONF_LD2450_ID])
    cg.add(var.set_ld2450(radar))
    ld2410_radar = await cg.get_variable(config[CONF_LD2410_ID])
    cg.add(var.set_ld2410(ld2410_radar))
    ld2410_uart = await cg.get_variable(config[CONF_LD2410_UART_ID])
    cg.add(var.set_ld2410_uart(ld2410_uart))
    cg.add_define("USE_UART_DEBUGGER")
    if config[CONF_SHADOW_TRANSPORT_SMOKE]:
        cg.add_define("NAMO_SHADOW_TRANSPORT_SMOKE")
    if config[CONF_PRODUCTION_FUSION]:
        cg.add_define("NAMO_PRODUCTION_FUSION")
    if config[CONF_SIGNED_OTA]:
        cg.add_define("NAMO_SIGNED_OTA")
