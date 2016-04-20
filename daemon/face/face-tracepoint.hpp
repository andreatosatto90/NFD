#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER faceLog

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "daemon/face/face-tracepoint.hpp"

#if !defined(NFD_DAEMON_FACE_FACE_TRACEPOINT_HPP) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define NFD_DAEMON_FACE_FACE_TRACEPOINT_HPP

#include <lttng/tracepoint.h>

TRACEPOINT_EVENT(
  faceLog,
  packet_sent,
  TP_ARGS(
    const char*, localEndpoint,
    const char*, remoteEndpoint,
    int, bytes
  ),
  TP_FIELDS(
    ctf_string(local_endpoint, localEndpoint)
    ctf_string(remote_endpoint, remoteEndpoint)
    ctf_integer(int, bytes, bytes)
  )
)

TRACEPOINT_EVENT(
  faceLog,
  packet_received,
  TP_ARGS(
    const char*, localEndpoint,
    const char*, remoteEndpoint,
    int, bytes
  ),
  TP_FIELDS(
    ctf_string(local_endpoint, localEndpoint)
    ctf_string(remote_endpoint, remoteEndpoint)
    ctf_integer(int, bytes, bytes)
  )
)

TRACEPOINT_EVENT(
  faceLog,
  packet_received_error,
  TP_ARGS(
    const char*, localEndpoint,
    const char*, remoteEndpoint,
    int, bytes,
    int, errorNum
  ),
  TP_FIELDS(
    ctf_string(local_endpoint, localEndpoint)
    ctf_string(remote_endpoint, remoteEndpoint)
    ctf_integer(int, bytes, bytes)
    ctf_integer(int, error, errorNum)
  )
)

TRACEPOINT_EVENT(
  faceLog,
  packet_sent_error,
  TP_ARGS(
    const char*, localEndpoint,
    const char*, remoteEndpoint,
    int, bytes,
    int, errorNum
  ),
  TP_FIELDS(
    ctf_string(local_endpoint, localEndpoint)
    ctf_string(remote_endpoint, remoteEndpoint)
    ctf_integer(int, bytes, bytes)
    ctf_integer(int, error, errorNum)
  )
)

#endif // NFD_DAEMON_FACE_FACE_TRACEPOINT_HPP

#include <lttng/tracepoint-event.h>
