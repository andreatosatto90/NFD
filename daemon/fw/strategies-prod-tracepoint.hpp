#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER strategyProdLog

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "daemon/fw/strategies-prod-tracepoint.hpp"

#if !defined(NFD_DAEMON_FW_STRATEGIES_PROD_TRACEPOINT_HPP) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define NFD_DAEMON_FW_STRATEGIES_PROD_TRACEPOINT_HPP

#include <lttng/tracepoint.h>


TRACEPOINT_EVENT(
  strategyProdLog,
  interest_received,
  TP_ARGS(
    const char*, interest
  ),
  TP_FIELDS(
    ctf_string(interest_name, interest)
  )
)

#endif // NFD_DAEMON_FW_STRATEGIES_PROD_TRACEPOINT_HPP

#include <lttng/tracepoint-event.h>
