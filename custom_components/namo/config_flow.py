from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.config_entries import ConfigFlowResult
from homeassistant.const import CONF_HOST
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .api import NamoApiClient, NamoApiError
from .const import CONF_SITE_ID, DOMAIN
from .routing import RouteContractError, normalize_device_url


class NamoConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 3

    def __init__(self) -> None:
        self._discovered_host: str | None = None
        self._discovered_site_id: str | None = None
        self._discovered_title: str | None = None

    async def _async_validate(self, host: str) -> tuple[str, str, str]:
        base_url = normalize_device_url(host)
        client = NamoApiClient(async_get_clientsession(self.hass))
        leader = await client.async_resolve_site_leader(base_url)
        return base_url, leader.site_id, leader.device_name

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                host, site_id, title = await self._async_validate(
                    user_input[CONF_HOST]
                )
            except (NamoApiError, RouteContractError):
                errors["base"] = "cannot_connect"
            else:
                await self.async_set_unique_id(site_id)
                self._abort_if_unique_id_configured(
                    updates={CONF_HOST: host, CONF_SITE_ID: site_id}
                )
                return self.async_create_entry(
                    title=title,
                    data={CONF_HOST: host, CONF_SITE_ID: site_id},
                )
        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema({vol.Required(CONF_HOST): str}),
            errors=errors,
        )

    async def async_step_zeroconf(
        self, discovery_info: ZeroconfServiceInfo
    ) -> ConfigFlowResult:
        try:
            host, site_id, title = await self._async_validate(
                discovery_info.host
            )
        except (NamoApiError, RouteContractError):
            return self.async_abort(reason="cannot_connect")
        await self.async_set_unique_id(site_id)
        self._abort_if_unique_id_configured(
            updates={CONF_HOST: host, CONF_SITE_ID: site_id}
        )
        self._discovered_host = host
        self._discovered_site_id = site_id
        self._discovered_title = title
        self.context["title_placeholders"] = {"name": title}
        return await self.async_step_zeroconf_confirm()

    async def async_step_zeroconf_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        if (
            self._discovered_host is None
            or self._discovered_site_id is None
            or self._discovered_title is None
        ):
            return self.async_abort(reason="cannot_connect")
        if user_input is not None:
            return self.async_create_entry(
                title=self._discovered_title,
                data={
                    CONF_HOST: self._discovered_host,
                    CONF_SITE_ID: self._discovered_site_id,
                },
            )
        return self.async_show_form(
            step_id="zeroconf_confirm",
            description_placeholders={"name": self._discovered_title},
        )
