from __future__ import annotations

from dataclasses import dataclass, replace

from .routing import GroupRoute, SpaceSnapshot, unavailable_snapshot


@dataclass(slots=True)
class _FailureWindow:
    started_at: float
    last_good: SpaceSnapshot
    source_age_at_start_ms: int


class SnapshotAvailabilityGuard:
    """Preserve a bounded last-known state across transient transport loss."""

    def __init__(self, grace_seconds: float) -> None:
        self._grace_seconds = grace_seconds
        self._failures: dict[str, _FailureWindow] = {}

    @staticmethod
    def _compatible_logical_route(
        route: GroupRoute, snapshot: SpaceSnapshot
    ) -> bool:
        return (
            route.site_id == snapshot.site_id
            and route.space_id == snapshot.space_id
            and route.group_id == snapshot.group_id
            and route.group_term >= snapshot.group_term
        )

    def accept(self, snapshot: SpaceSnapshot) -> None:
        self._failures.pop(snapshot.space_id, None)

    def reject(
        self,
        route: GroupRoute,
        previous: SpaceSnapshot | None,
        *,
        reason: str,
        now: float,
    ) -> tuple[SpaceSnapshot, bool]:
        window = self._failures.get(route.space_id)
        if window is not None and not self._compatible_logical_route(
            route, window.last_good
        ):
            self._failures.pop(route.space_id, None)
            window = None

        if window is None:
            if (
                previous is None
                or not previous.available
                or not self._compatible_logical_route(route, previous)
            ):
                return (
                    unavailable_snapshot(route, previous, reason=reason),
                    False,
                )
            source_age_ms = previous.source_age_ms or 0
            started_at = now
            if previous.continuity_held:
                grace_ms = max(0, int(self._grace_seconds * 1000))
                remaining_ms = min(
                    grace_ms, max(0, previous.hold_remaining_ms or 0)
                )
                if remaining_ms == 0:
                    return (
                        unavailable_snapshot(route, previous, reason=reason),
                        False,
                    )
                elapsed_before_ms = grace_ms - remaining_ms
                started_at -= elapsed_before_ms / 1000
                source_age_ms = max(0, source_age_ms - elapsed_before_ms)
            window = _FailureWindow(started_at, previous, source_age_ms)
            self._failures[route.space_id] = window

        elapsed = max(0.0, now - window.started_at)
        if elapsed < self._grace_seconds:
            return (
                replace(
                    window.last_good,
                    health="degraded",
                    unavailable_reason=reason,
                    continuity_held=True,
                    hold_remaining_ms=max(
                        0, int((self._grace_seconds - elapsed) * 1000)
                    ),
                    source_age_ms=(
                        window.source_age_at_start_ms + int(elapsed * 1000)
                    ),
                ),
                True,
            )
        return (
            unavailable_snapshot(
                route,
                window.last_good,
                reason=reason,
            ),
            False,
        )

    def discard(self, space_id: str) -> None:
        self._failures.pop(space_id, None)

    def retain(self, space_ids: set[str]) -> None:
        for space_id in tuple(self._failures):
            if space_id not in space_ids:
                self._failures.pop(space_id, None)
