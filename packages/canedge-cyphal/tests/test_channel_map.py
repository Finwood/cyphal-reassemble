import pytest
from canedge_cyphal.reassemble import (
    ChannelMapError,
    assign_channel,
    compile_channel_map,
)


def test_no_map_omits_rti():
    compiled = compile_channel_map(None)
    assert assign_channel(compiled, "CAN1") == ("CAN1", None)


def test_one_entry_map_uses_bus_name_and_rti_zero():
    compiled = compile_channel_map({"aux": ["CAN3"]})
    assert compiled.use_rti is True
    assert assign_channel(compiled, "CAN3") == ("aux", 0)


def test_unlisted_channel_keeps_its_name_and_rti_zero():
    compiled = compile_channel_map({"flight": ["CAN1", "CAN2"]})
    assert assign_channel(compiled, "CAN1") == ("flight", 0)
    assert assign_channel(compiled, "CAN2") == ("flight", 1)
    assert assign_channel(compiled, "CAN9") == ("CAN9", 0)


def test_map_rejects_257_channels_and_duplicates():
    with pytest.raises(ChannelMapError):
        compile_channel_map({"bus": [f"C{i}" for i in range(257)]})
    with pytest.raises(ChannelMapError):
        compile_channel_map({"a": ["CAN1"], "b": ["CAN1"]})
    with pytest.raises(ChannelMapError):
        compile_channel_map({"a": ["CAN1", "CAN1"]})
