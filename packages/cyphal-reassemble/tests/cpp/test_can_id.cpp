#include <gtest/gtest.h>
extern "C" {
#include "can_id.h"
}

TEST(CanId, HeartbeatMessage) {
    cy_can_id_t d = cy_decode_can_id(0x101D552AU);
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.kind, CY_KIND_MESSAGE);
    EXPECT_EQ(d.port_id, 7509);
    EXPECT_EQ(d.priority, 4);
    EXPECT_EQ(d.source_node_id, 42);
    EXPECT_EQ(d.dest_node_id, CY_NODE_ID_UNSET);
}

TEST(CanId, ServiceResponse) {
    cy_can_id_t d = cy_decode_can_id(0x126B957DU);
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.kind, CY_KIND_RESPONSE);
    EXPECT_EQ(d.port_id, 430);
    EXPECT_EQ(d.source_node_id, 125);
    EXPECT_EQ(d.dest_node_id, 42);
}

TEST(CanId, ServiceRequest) {
    cy_can_id_t d = cy_decode_can_id(0x136BBEAAU);
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.kind, CY_KIND_REQUEST);
    EXPECT_EQ(d.port_id, 430);
    EXPECT_EQ(d.source_node_id, 42);
    EXPECT_EQ(d.dest_node_id, 125);
}

TEST(CanId, AnonymousMessageHasUnsetSource) {
    // Heartbeat layout with the anonymous flag (bit 24) set.
    cy_can_id_t d = cy_decode_can_id(0x101D552AU | (1U << 24));
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.kind, CY_KIND_MESSAGE);
    EXPECT_EQ(d.source_node_id, CY_NODE_ID_UNSET);
}

TEST(CanId, ServiceWithEqualSourceAndDestIsInvalid) {
    // service 430, dest 42, source 42 -> invalid per spec
    uint32_t id = (4U << 26) | (1U << 25) | (430U << 14) | (42U << 7) | 42U;
    cy_can_id_t d = cy_decode_can_id(id);
    EXPECT_FALSE(d.valid);
}

TEST(CanId, MessageWithReservedBit7IsInvalid) {
    cy_can_id_t d = cy_decode_can_id(0x101D552AU | (1U << 7));
    EXPECT_FALSE(d.valid);
}
