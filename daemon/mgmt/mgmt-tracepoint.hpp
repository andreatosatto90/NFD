#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER mgmtLog

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "daemon/mgmt/mgmt-tracepoint.hpp"

#if !defined(NFD_DAEMON_MGMT_MGMT_TRACEPOINT_HPP) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define NFD_DAEMON_MGMT_MGMT_TRACEPOINT_HPP

#include <lttng/tracepoint.h>

TRACEPOINT_EVENT(
  mgmtLog,
  network_state,
  TP_ARGS(
    const char*, interfaceName,
    const char*, interfaceState
  ),
  TP_FIELDS(
    ctf_string(interface_name, interfaceName)
    ctf_string(interface_state, interfaceState)
  )
)

TRACEPOINT_EVENT(
  mgmtLog,
  address_added,
  TP_ARGS(
    const char*, interfaceName,
    const char*, interfaceAddress
  ),
  TP_FIELDS(
    ctf_string(interface_name, interfaceName)
    ctf_string(interface_address, interfaceAddress)
  )
)

TRACEPOINT_EVENT(
  mgmtLog,
  address_removed,
  TP_ARGS(
    const char*, interfaceName,
    const char*, interfaceAddress
  ),
  TP_FIELDS(
    ctf_string(interface_name, interfaceName)
    ctf_string(interface_address, interfaceAddress)
  )
)

#endif // NFD_DAEMON_MGMT_MGMT_TRACEPOINT_HPP

#include <lttng/tracepoint-event.h>
