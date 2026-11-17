#ifndef CYPHAL_REASSEMBLE_CAN_ID_H
#define CYPHAL_REASSEMBLE_CAN_ID_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CY_NODE_ID_UNSET 255U

typedef enum {
    CY_KIND_MESSAGE = 0,
    CY_KIND_RESPONSE = 1,
    CY_KIND_REQUEST = 2
} cy_transfer_kind_t;

typedef struct {
    bool valid;                 /* false if the ID is not a valid Cyphal/CAN frame layout */
    cy_transfer_kind_t kind;
    uint16_t port_id;           /* subject-id (message) or service-id (service) */
    uint8_t priority;           /* 0..7 */
    uint8_t source_node_id;     /* 0..127, or CY_NODE_ID_UNSET if anonymous */
    uint8_t dest_node_id;       /* 0..127 for services; CY_NODE_ID_UNSET for messages */
} cy_can_id_t;

/* Decode a 29-bit extended CAN ID into its Cyphal/CAN fields. */
cy_can_id_t cy_decode_can_id(uint32_t extended_can_id);

#ifdef __cplusplus
}
#endif

#endif
