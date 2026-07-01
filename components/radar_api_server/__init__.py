import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server_base
from esphome.const import CONF_ID

DEPENDENCIES = ["web_server_base"]

CONF_WEB_SERVER_BASE_ID = "web_server_base_id"

radar_api_server_ns = cg.esphome_ns.namespace("radar_api_server")
RadarApiServer = radar_api_server_ns.class_("RadarApiServer", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(RadarApiServer),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], base)
    await cg.register_component(var, config)
