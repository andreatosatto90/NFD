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
    const char*, interfaceName,
    int, retryTimeout,
    int, isNew
  ),
  TP_FIELDS(
    ctf_string(strategy_name, strategyName)
    ctf_string(interest_name, interest)
    ctf_integer(int, face_id, faceId)
    ctf_string(interface_name, interfaceName)
    ctf_integer(int, retry_timeout, retryTimeout)
    ctf_integer(int, is_new, isNew)
  )
)

TRACEPOINT_EVENT(
  strategyLog,
  interest_sent_retry,
  TP_ARGS(
    const char*, strategyName,
    const char*, interest,
    int, faceId,
    const char*, interfaceName,
    int, retryTimeout
  ),
  TP_FIELDS(
    ctf_string(strategy_name, strategyName)
    ctf_string(interest_name, interest)
    ctf_integer(int, face_id, faceId)
    ctf_string(interface_name, interfaceName)
    ctf_integer(int, retry_timeout, retryTimeout)
  )
)

TRACEPOINT_EVENT(
  strategyLog,
  data_received,
  TP_ARGS(
    const char*, strategyName,
    const char*, interest,
    int, faceId,
    const char*, interfaceName,
    int, rtt,
    int, meanRtt,
    int, nRetries,
    int, retrieveTime,
    int, boundedRtt
  ),
  TP_FIELDS(
    ctf_string(strategy_name, strategyName)
    ctf_string(interest_name, interest)
    ctf_integer(int, face_id, faceId)
    ctf_string(interface_name, interfaceName)
    ctf_integer(int, rtt, rtt)
    ctf_integer(int, mean_rtt, meanRtt)
    ctf_integer(int, num_retries, nRetries)
    ctf_integer(int, retrieve_time, retrieveTime)
    ctf_integer(int, bounded_rtt, boundedRtt)
  )
)

TRACEPOINT_EVENT(
  strategyLog,
  data_rejected,
  TP_ARGS(
    const char*, strategyName,
    const char*, interest,
    int, faceId,
    const char*, interfaceName,
    int, rtt,
    int, meanRtt,
    int, nRetries,
    int, retrieveTime,
    int, boundedRtt
  ),
  TP_FIELDS(
    ctf_string(strategy_name, strategyName)
    ctf_string(interest_name, interest)
    ctf_integer(int, face_id, faceId)
    ctf_string(interface_name, interfaceName)
    ctf_integer(int, rtt, rtt)
    ctf_integer(int, mean_rtt, meanRtt)
    ctf_integer(int, num_retries, nRetries)
    ctf_integer(int, retrieve_time, retrieveTime)
    ctf_integer(int, bounded_rtt, boundedRtt)
  )
)

TRACEPOINT_EVENT(
  strategyLog,
  rtt_min,
  TP_ARGS(
    int, val
  ),
  TP_FIELDS(
    ctf_integer(int, rtt_min, val)
  )
)

TRACEPOINT_EVENT(
  strategyLog,
  rtt_max,
  TP_ARGS(
    int, val
  ),
  TP_FIELDS(
    ctf_integer(int, rtt_max, val)
  )
)

TRACEPOINT_EVENT(
  strategyLog,
  rtt_min_calc,
  TP_ARGS(
    int, val
  ),
  TP_FIELDS(
    ctf_integer(int, rtt_min_calc, val)
  )
)

#endif // NFD_DAEMON_FW_STRATEGIES_TRACEPOINT_HPP

#include <lttng/tracepoint-event.h>
