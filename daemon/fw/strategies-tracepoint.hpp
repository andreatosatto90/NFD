#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER strategyLog

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "daemon/fw/strategies-tracepoint.hpp"

#if !defined(NFD_DAEMON_FW_STRATEGIES_TRACEPOINT_HPP) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define NFD_DAEMON_FW_STRATEGIES_TRACEPOINT_HPP

#include <lttng/tracepoint.h>


TRACEPOINT_EVENT(
  strategyLog,
  interest_sent,
  TP_ARGS(
    const char*, strategyName,
    const char*, interest,
    int, faceId,
    const char*, interfaceName
  ),
  TP_FIELDS(
    ctf_string(strategy_name, strategyName)
    ctf_string(interest_name, interest)
    ctf_integer(int, face_id, faceId)
    ctf_string(interface_name, interfaceName)
  )
)

TRACEPOINT_EVENT(
  strategyLog,
  data_received,
  TP_ARGS(
    const char*, strategyName,
    const char*, interest,
    int, faceId,
    const char*, interfaceName
  ),
  TP_FIELDS(
    ctf_string(strategy_name, strategyName)
    ctf_string(interest_name, interest)
    ctf_integer(int, face_id, faceId)
    ctf_string(interface_name, interfaceName)
  )
)

TRACEPOINT_EVENT(
  strategyLog,
  interface_state_changed,
  TP_ARGS(
    const char*, interfaceName,
    const char*, interfaceState
  ),
  TP_FIELDS(
    ctf_string(interface_name, interfaceName)
    ctf_string(interface_state, interfaceState)
  )
)


#endif // NFD_DAEMON_FW_STRATEGIES_TRACEPOINT_HPP

#include <lttng/tracepoint-event.h>
