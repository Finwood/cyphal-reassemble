from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass


class ChannelMapError(ValueError):
    """The channel map cannot be applied."""


@dataclass(frozen=True, slots=True)
class CompiledMap:
    use_rti: bool
    assignment: dict[str, tuple[str, int]]


def compile_channel_map(raw: Mapping[str, Sequence[str]] | None) -> CompiledMap:
    if raw is None:
        return CompiledMap(use_rti=False, assignment={})
    assignment: dict[str, tuple[str, int]] = {}
    for bus, channels in raw.items():
        if len(channels) > 256:
            raise ChannelMapError(
                f"bus {bus!r} has {len(channels)} channels; the maximum is 256"
            )
        for index, channel in enumerate(channels):
            if channel in assignment:
                raise ChannelMapError(f"channel {channel!r} is listed more than once")
            assignment[channel] = (str(bus), index)
    return CompiledMap(use_rti=True, assignment=assignment)


def assign_channel(compiled: CompiledMap, channel: str) -> tuple[str, int | None]:
    if not compiled.use_rti:
        return channel, None
    found = compiled.assignment.get(channel)
    if found is None:
        return channel, 0
    return found