#include "can_id.h"

#define OFFSET_PRIORITY 26U
#define OFFSET_SUBJECT_ID 8U
#define OFFSET_SERVICE_ID 14U
#define OFFSET_DST_NODE_ID 7U

#define FLAG_SERVICE_NOT_MESSAGE  (UINT32_C(1) << 25U)
#define FLAG_ANONYMOUS_MESSAGE    (UINT32_C(1) << 24U)
#define FLAG_REQUEST_NOT_RESPONSE (UINT32_C(1) << 24U)
#define FLAG_RESERVED_23          (UINT32_C(1) << 23U)
#define FLAG_RESERVED_07          (UINT32_C(1) << 7U)

#define PRIORITY_MAX 7U
#define NODE_ID_MAX 127U
#define SUBJECT_ID_MAX 8191U
#define SERVICE_ID_MAX 511U

cy_can_id_t cy_decode_can_id(uint32_t can_id)
{
    cy_can_id_t out;
    out.valid = false;
    out.kind = CY_KIND_MESSAGE;
    out.port_id = 0U;
    out.priority = (uint8_t)((can_id >> OFFSET_PRIORITY) & PRIORITY_MAX);
    out.source_node_id = (uint8_t)(can_id & NODE_ID_MAX);
    out.dest_node_id = CY_NODE_ID_UNSET;

    if (0U == (can_id & FLAG_SERVICE_NOT_MESSAGE)) {
        out.kind = CY_KIND_MESSAGE;
        out.port_id = (uint16_t)((can_id >> OFFSET_SUBJECT_ID) & SUBJECT_ID_MAX);
        if ((can_id & FLAG_ANONYMOUS_MESSAGE) != 0U) {
            out.source_node_id = (uint8_t)CY_NODE_ID_UNSET;
        }
        out.dest_node_id = (uint8_t)CY_NODE_ID_UNSET;
        out.valid = (0U == (can_id & FLAG_RESERVED_23)) && (0U == (can_id & FLAG_RESERVED_07));
    } else {
        out.kind = ((can_id & FLAG_REQUEST_NOT_RESPONSE) != 0U) ? CY_KIND_REQUEST : CY_KIND_RESPONSE;
        out.port_id = (uint16_t)((can_id >> OFFSET_SERVICE_ID) & SERVICE_ID_MAX);
        out.dest_node_id = (uint8_t)((can_id >> OFFSET_DST_NODE_ID) & NODE_ID_MAX);
        out.valid = (0U == (can_id & FLAG_RESERVED_23)) && (out.source_node_id != out.dest_node_id);
    }
    return out;
}
