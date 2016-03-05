#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER strategyLog

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "daemon/fw/strategies-tracepoint.hpp"

#if !defined(NFD_DAEMON_FW_STRATEGIES_TRACEPOINT_HPP) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define NFD_DAEMON_FW_STRATEGIES_TRACEPOINT_HPP

#include <lttng/tracepoint.h>


TRACEPOINT_EVENT(
    strategyLog,
    weighted_random,
    TP_ARGS(
        int, faceId,
        const char*, strategyName
    ),
    TP_FIELDS(
        ctf_integer(int, face_id, faceId)
        ctf_string(strategy_name, strategyName)
    )
)

#endif // NFD_DAEMON_FW_STRATEGIES_TRACEPOINT_HPP

#include <lttng/tracepoint-event.h>
