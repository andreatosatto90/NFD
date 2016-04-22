/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2014-2015,  Regents of the University of California,
 *                           Arizona Board of Regents,
 *                           Colorado State University,
 *                           University Pierre & Marie Curie, Sorbonne University,
 *                           Washington University in St. Louis,
 *                           Beijing Institute of Technology,
 *                           The University of Memphis.
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef NFD_DAEMON_FACE_DATAGRAM_TRANSPORT_HPP
#define NFD_DAEMON_FACE_DATAGRAM_TRANSPORT_HPP

#include "transport.hpp"
#include "core/global-io.hpp"
#include "face-tracepoint.hpp"

#include <array>

namespace nfd {

namespace ip = boost::asio::ip;

namespace udp {
typedef boost::asio::ip::udp::endpoint Endpoint;
} // namespace udp

namespace face {

struct Unicast {};
struct Multicast {};

/** \brief Implements Transport for datagram-based protocols.
 *
 *  \tparam Protocol a datagram-based protocol in Boost.Asio
 */
template<class Protocol, class Addressing = Unicast>
class DatagramTransport : public Transport
{
public:
  typedef Protocol protocol;

  /** \brief Construct datagram transport.
   *
   *  \param socket Protocol-specific socket for the created transport
   */
  explicit
  DatagramTransport(typename protocol::socket&& socket);

  /** \brief Construct datagram transport.
   */
  explicit
  DatagramTransport(typename protocol::endpoint remoteEndpoint);

  /** \brief Receive datagram, translate buffer into packet, deliver to parent class.
   */
  void
  receiveDatagram(const uint8_t* buffer, size_t nBytesReceived,
                  const boost::system::error_code& error);

protected:
  bool
  rebindSocket(typename protocol::endpoint localEndpoint);

  /*bool
  connectSocket();*/

  virtual void
  doClose() DECL_OVERRIDE;

  virtual void
  doSend(Transport::Packet&& packet) DECL_OVERRIDE;

  void
  doConnect();

  void
  handleConnect(const boost::system::error_code& error);

  void
  handleSend(const boost::system::error_code& error,
             size_t nBytesSent, const Block& payload);

  void
  handleReceive(const boost::system::error_code& error,
                size_t nBytesReceived);

  void
  processErrorCode(const boost::system::error_code& error);

  bool
  hasBeenUsedRecently() const;

  void
  resetRecentUsage();

  static EndpointId
  makeEndpointId(const typename protocol::endpoint& ep);

protected:
  typename protocol::socket m_socket;
  unique_ptr<typename protocol::socket> m_socket2;
  typename protocol::endpoint m_sender;

  NFD_LOG_INCLASS_DECLARE();

private:
  std::array<uint8_t, ndn::MAX_NDN_PACKET_SIZE> m_receiveBuffer;
  bool m_hasBeenUsedRecently;
  typename protocol::endpoint m_remoteEndpoint;
  typename protocol::endpoint m_localEndpoint;
  bool m_isConnected;
  bool m_inConnection;
};


template<class T, class U>
DatagramTransport<T, U>::DatagramTransport(typename DatagramTransport::protocol::socket&& socket)
  : m_socket(std::move(socket))
  , m_hasBeenUsedRecently(false) // TODO add remote endpoint
{
  m_socket.async_receive_from(boost::asio::buffer(m_receiveBuffer), m_sender,
                              bind(&DatagramTransport<T, U>::handleReceive, this,
                                   boost::asio::placeholders::error,
                                   boost::asio::placeholders::bytes_transferred));

  // TODO set class local and remote endpoint
}

template<class T, class U>
DatagramTransport<T, U>::DatagramTransport(typename protocol::endpoint remoteEndpoint)
  : m_socket(getGlobalIoService(), remoteEndpoint.protocol())
  , m_hasBeenUsedRecently(false)
  , m_remoteEndpoint(remoteEndpoint)
{
  m_isConnected = false;
  m_inConnection = false;
}

template<class T, class U>
void
DatagramTransport<T, U>::doClose()
{
  NFD_LOG_FACE_TRACE(__func__);

  if (m_socket.is_open()) {
    // Cancel all outstanding operations and close the socket.
    // Use the non-throwing variants and ignore errors, if any.
    boost::system::error_code error;
    m_socket.cancel(error);
    m_socket.close(error);
  }

  // Ensure that the Transport stays alive at least until
  // all pending handlers are dispatched
  getGlobalIoService().post([this] {
    this->setState(TransportState::CLOSED);
  });
}

template<class T, class U>
void
DatagramTransport<T, U>::doSend(Transport::Packet&& packet)
{
  NFD_LOG_FACE_TRACE(__func__);

//  if (!m_socket.is_open()) {
//    // Rebind socket
//    typename protocol::endpoint defaultEndpoint; // TODO not good
//    if (m_localEndpoint != defaultEndpoint)
//      rebindSocket(m_localEndpoint);
//  }

  if (m_socket.is_open()) {

    if (!m_isConnected && !m_inConnection) {
      m_inConnection = true;
      m_socket.async_connect(m_remoteEndpoint, bind(&DatagramTransport<T, U>::handleConnect, this,
                                                    boost::asio::placeholders::error));
    }

    m_socket.async_send(boost::asio::buffer(packet.packet),
                        bind(&DatagramTransport<T, U>::handleSend, this,
                             boost::asio::placeholders::error,
                             boost::asio::placeholders::bytes_transferred,
                             packet.packet));
  }
}

template<class T, class U>
void
DatagramTransport<T, U>::doConnect()
{

}

template<class T, class U>
void
DatagramTransport<T, U>::receiveDatagram(const uint8_t* buffer, size_t nBytesReceived,
                                         const boost::system::error_code& error)
{
  if (error)
    return processErrorCode(error);

  NFD_LOG_FACE_TRACE("Received from : "<< m_remoteEndpoint << " -> " << nBytesReceived << " bytes");

  // TODO better conversion
  std::ostringstream local;
  local << m_localEndpoint;
  std::ostringstream remote;
  remote << m_remoteEndpoint;

  bool isOk = false;
  Block element;
  std::tie(isOk, element) = Block::fromBuffer(buffer, nBytesReceived);
  if (!isOk) {
    NFD_LOG_FACE_WARN("Failed to parse incoming packet");
    tracepoint(faceLog, packet_received_error, local.str().c_str(), remote.str().c_str(), nBytesReceived, 1);
    // This packet won't extend the face lifetime
    return;
  }
  if (element.size() != nBytesReceived) {
    NFD_LOG_FACE_WARN("Received datagram size and decoded element size don't match E: " << element.size() << " R: " <<  nBytesReceived);
    tracepoint(faceLog, packet_received_error, local.str().c_str(), remote.str().c_str(), nBytesReceived, 2);
    // This packet won't extend the face lifetime
    return;
  }
  m_hasBeenUsedRecently = true;


  tracepoint(faceLog, packet_received, local.str().c_str(), remote.str().c_str(), nBytesReceived);

  Transport::Packet tp(std::move(element));
  tp.remoteEndpoint = makeEndpointId(m_sender);
  this->receive(std::move(tp));
}

template<class T, class U>
bool
DatagramTransport<T, U>::rebindSocket(typename protocol::endpoint localEndpoint)

{
  m_localEndpoint = localEndpoint; // TODO We need this to retry the socket binding (is it useful?)

  if (m_socket.is_open()) {
    // Cancel all outstanding operations and close the socket.
    // Use the non-throwing variants and ignore errors, if any.
    boost::system::error_code error;
    m_socket.cancel(error);
    m_socket.close(error);
  }

  m_socket = ip::udp::socket(getGlobalIoService(), m_localEndpoint.protocol());
  m_socket.set_option(ip::udp::socket::reuse_address(true));
  m_isConnected = false;

  try {
    m_socket.bind(m_localEndpoint);
  }
  catch (const boost::system::system_error& e) {
    NFD_LOG_FACE_ERROR("Error binding socket for interface face from " << m_localEndpoint
                       << " to " << m_remoteEndpoint << ": " << e.what());
   // return false; // TODO Uncomment to have a new local address if possible
  }

  m_socket.async_connect(m_remoteEndpoint, bind(&DatagramTransport<T, U>::handleConnect, this,
                                                boost::asio::placeholders::error));
  return true;
}

/*template<class T, class U>
bool
DatagramTransport<T, U>::connectSocket()
{
  try {
    m_socket.connect(m_remoteEndpoint);

    m_socket.async_receive_from(boost::asio::buffer(m_receiveBuffer), m_sender,
                                bind(&DatagramTransport<T, U>::handleReceive, this,
                                     boost::asio::placeholders::error,
                                     boost::asio::placeholders::bytes_transferred));
  }
  catch (const boost::system::system_error& e) {
    NFD_LOG_FACE_ERROR("Error connecting socket for interface face from " << m_localEndpoint
                       << " to " << m_remoteEndpoint << ": " << e.what());
    return false;
  }

  return true;
}*/

template<class T, class U>
void
DatagramTransport<T, U>::handleConnect(const boost::system::error_code& error)
{
  m_inConnection = false;

  if (error) {
    NFD_LOG_FACE_ERROR("Error connecting socket for interface face from " << m_localEndpoint
                       << " to " << m_remoteEndpoint << ": " << error.message());
    m_isConnected = false;
    return;
  }

  m_isConnected = true;

  m_socket.async_receive_from(boost::asio::buffer(m_receiveBuffer), m_sender,
                              bind(&DatagramTransport<T, U>::handleReceive, this,
                                   boost::asio::placeholders::error,
                                   boost::asio::placeholders::bytes_transferred));
}

template<class T, class U>
void
DatagramTransport<T, U>::handleReceive(const boost::system::error_code& error,
                                       size_t nBytesReceived)
{
  receiveDatagram(m_receiveBuffer.data(), nBytesReceived, error);

  if (m_socket.is_open())
    m_socket.async_receive_from(boost::asio::buffer(m_receiveBuffer), m_sender,
                                bind(&DatagramTransport<T, U>::handleReceive, this,
                                     boost::asio::placeholders::error,
                                     boost::asio::placeholders::bytes_transferred));
}

template<class T, class U>
void
DatagramTransport<T, U>::handleSend(const boost::system::error_code& error,
                                    size_t nBytesSent, const Block& payload)
// 'payload' is unused; it's needed to retain the underlying Buffer
{
  // TODO better conversion
  std::ostringstream local;
  local << m_localEndpoint;
  std::ostringstream remote;
  remote << m_remoteEndpoint;

  if (error) {
    NFD_LOG_FACE_DEBUG(" NOT sent - Error socket");
    tracepoint(faceLog, packet_sent_error, local.str().c_str(), remote.str().c_str(), nBytesSent, 1);
    return processErrorCode(error);
  }

  if (!m_isConnected) {
    NFD_LOG_FACE_DEBUG(" NOT sent - Connection error ");
    tracepoint(faceLog, packet_sent_error, local.str().c_str(), remote.str().c_str(), nBytesSent, 2);
  }
  else {
    //NFD_LOG_FACE_DEBUG("Successfully sent: " << nBytesSent << " bytes");
    tracepoint(faceLog, packet_sent, local.str().c_str(), remote.str().c_str(), nBytesSent);
  }
}

template<class T, class U>
void
DatagramTransport<T, U>::processErrorCode(const boost::system::error_code& error)
{
  NFD_LOG_FACE_TRACE(__func__);

  if (getState() == TransportState::CLOSING ||
      getState() == TransportState::FAILED ||
      getState() == TransportState::CLOSED ||
      error == boost::asio::error::operation_aborted)
    // transport is shutting down, ignore any errors
    return;

  if (getPersistency() == ndn::nfd::FacePersistency::FACE_PERSISTENCY_PERMANENT) {
    //NFD_LOG_FACE_DEBUG("Permanent face ignores error: " << error.message());
    return;
  }

  if (error != boost::asio::error::eof)
    NFD_LOG_FACE_WARN("Send or receive operation failed: " << error.message());

  this->setState(TransportState::FAILED);
  doClose();
}

template<class T, class U>
inline bool
DatagramTransport<T, U>::hasBeenUsedRecently() const
{
  return m_hasBeenUsedRecently;
}

template<class T, class U>
inline void
DatagramTransport<T, U>::resetRecentUsage()
{
  m_hasBeenUsedRecently = false;
}

template<class T, class U>
inline Transport::EndpointId
DatagramTransport<T, U>::makeEndpointId(const typename protocol::endpoint& ep)
{
  return 0;
}

} // namespace face
} // namespace nfd

#endif // NFD_DAEMON_FACE_DATAGRAM_TRANSPORT_HPP
