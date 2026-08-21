from __future__ import annotations

from time import monotonic

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers import issue_registry as ir

from .const import DOMAIN, ISSUE_DELAY_SECONDS


class NamoIssueTracker:
    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        self._hass = hass
        self._entry = entry
        self._unhealthy_since: float | None = None
        self._published_signature: tuple[str, ...] | None = None
        self._known_clear = False

    @property
    def issue_id(self) -> str:
        return f"{self._entry.entry_id}_site_unhealthy"

    def __call__(self) -> None:
        coordinator = self._entry.runtime_data
        data = coordinator.data
        site = None if data is None else data.site
        if (
            coordinator.last_update_success
            and site is not None
            and site.health == "healthy"
        ):
            self._unhealthy_since = None
            if not self._known_clear:
                ir.async_delete_issue(self._hass, DOMAIN, self.issue_id)
                self._published_signature = None
                self._known_clear = True
            return

        self._known_clear = False
        now = monotonic()
        if self._unhealthy_since is None:
            self._unhealthy_since = now
            return
        if now - self._unhealthy_since < ISSUE_DELAY_SECONDS:
            return

        problem_spaces = (
            site.problem_spaces
            if site is not None and site.problem_spaces
            else (self._entry.title,)
        )
        signature = tuple(problem_spaces)
        if signature == self._published_signature:
            return
        ir.async_create_issue(
            self._hass,
            DOMAIN,
            self.issue_id,
            is_fixable=False,
            severity=ir.IssueSeverity.WARNING,
            translation_key="site_unhealthy",
            translation_placeholders={
                "site": self._entry.title,
                "spaces": ", ".join(problem_spaces),
            },
        )
        self._published_signature = signature


def delete_site_issue(hass: HomeAssistant, entry: ConfigEntry) -> None:
    ir.async_delete_issue(
        hass,
        DOMAIN,
        f"{entry.entry_id}_site_unhealthy",
    )
