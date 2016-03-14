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

#include "unicast-udp-transport.hpp"
#include "udp-protocol.hpp"

#ifdef __linux__
#include <cerrno>       // for errno
#include <cstring>      // for std::strerror()
#include <netinet/in.h> // for IP_MTU_DISCOVER and IP_PMTUDISC_DONT
#include <sys/socket.h> // for setsockopt()
#endif

namespace nfd {
namespace face {

NFD_LOG_INCLASS_TEMPLATE_SPECIALIZATION_DEFINE(DatagramTransport, UnicastUdpTransport::protocol,
                                               "UnicastUdpTransport");

UnicastUdpTransport::UnicastUdpTransport(protocol::socket&& socket,
                                         ndn::nfd::FacePersistency persistency,
                                         time::nanoseconds idleTimeout,
                                         const shared_ptr<ndn::util::NetworkInterface>& ni)
  : DatagramTransport(std::move(socket)) // TODO what if we don't want to specify a socket?
  , m_idleTimeout(idleTimeout)
  , m_networkInterface(ni)
  , m_hasAddress(false)
{ 
  this->setLocalUri(FaceUri(m_socket.local_endpoint()));

  this->setRemoteUri(FaceUri(m_socket.remote_endpoint()));
  this->setScope(ndn::nfd::FACE_SCOPE_NON_LOCAL);
  this->setPersistency(persistency);
  this->setLinkType(ndn::nfd::LINK_TYPE_POINT_TO_POINT);
  this->setMtu(udp::computeMtu(m_socket.local_endpoint()));

  m_hasAddress =true;

  NFD_LOG_FACE_INFO("Creating transport");

#ifdef __linux__
  //
  // By default, Linux does path MTU discovery on IPv4 sockets,
  // and sets the DF (Don't Fragment) flag on datagrams smaller
  // than the interface MTU. However this does not work for us,
  // because we cannot properly respond to ICMP "packet too big"
  // messages by fragmenting the packet at the application level,
  // since we want to rely on IP for fragmentation and reassembly.
  //
  // Therefore, we disable PMTU discovery, which prevents the kernel
  // from setting the DF flag on outgoing datagrams, and thus allows
  // routers along the path to perform fragmentation as needed.
  //
  const int value = IP_PMTUDISC_DONT;
  if (::setsockopt(m_socket.native_handle(), IPPROTO_IP,
                   IP_MTU_DISCOVER, &value, sizeof(value)) < 0) {
    NFD_LOG_FACE_WARN("Failed to disable path MTU discovery: " << std::strerror(errno));
  }
#endif

  if (getPersistency() == ndn::nfd::FACE_PERSISTENCY_ON_DEMAND &&
      m_idleTimeout > time::nanoseconds::zero()) {
    scheduleClosureWhenIdle();
  }

  m_networkInterface->onStateChanged.connect(bind(&UnicastUdpTransport::changeStateFromInterface,
                                                  this, _2));
}

UnicastUdpTransport::UnicastUdpTransport(short localEndpointPort,
                                         udp::Endpoint remoteEndpoint,
                                         const shared_ptr<ndn::util::NetworkInterface>& ni)
  : DatagramTransport(remoteEndpoint) // TODO what if we don't want to specify a socket?
  , m_networkInterface(ni)
  , m_hasAddress(false)
  , m_localEndpointPort(localEndpointPort)
{
  // Set local URI
  std::string localUri;
  remoteEndpoint.address().is_v6() ? localUri = "udp6://" : localUri = "udp4://";
  localUri += ni->getName() + ":" + std::to_string(localEndpointPort);
  this->setLocalUri(FaceUri(localUri));

  this->setRemoteUri(FaceUri(remoteEndpoint));
  this->setScope(ndn::nfd::FACE_SCOPE_NON_LOCAL);
  this->setPersistency(ndn::nfd::FacePersistency::FACE_PERSISTENCY_PERMANENT);
  this->setLinkType(ndn::nfd::LINK_TYPE_POINT_TO_POINT);
  this->setMtu(ni->getMtu());

  NFD_LOG_FACE_INFO("Creating transport");

  m_networkInterface->onStateChanged.connect(bind(&UnicastUdpTransport::changeStateFromInterface, this, _2));
  m_networkInterface->onAddressAdded.connect(bind(&UnicastUdpTransport::handleAddressAdded, this, _1));
  m_networkInterface->onAddressRemoved.connect(bind(&UnicastUdpTransport::handleAddressRemoved, this, _1));

  changeSocketLocalAddress();;
}

void
UnicastUdpTransport::beforeChangePersistency(ndn::nfd::FacePersistency newPersistency)
{
  if (newPersistency == ndn::nfd::FACE_PERSISTENCY_ON_DEMAND &&
      m_idleTimeout > time::nanoseconds::zero()) {
    scheduleClosureWhenIdle();
  }
  else {
    m_closeIfIdleEvent.cancel();
    setExpirationTime(time::steady_clock::TimePoint::max());
  }
}

void
UnicastUdpTransport::changeStateFromInterface(ndn::util::NetworkInterfaceState state)
{
  if (getState() != TransportState::CLOSING && getState() != TransportState::CLOSED) {
    switch (state) {
      case ndn::util::NetworkInterfaceState::RUNNING:
        NFD_LOG_FACE_DEBUG("Changing state UP");
        setState(TransportState::UP);
        break;
      default:
        NFD_LOG_FACE_DEBUG("Changing state DOWN");
        setState(TransportState::DOWN);
        break;
    }
  }
}

void
UnicastUdpTransport::scheduleClosureWhenIdle()
{
  m_closeIfIdleEvent = scheduler::schedule(m_idleTimeout, [this] {
    if (!hasBeenUsedRecently()) {
      NFD_LOG_FACE_INFO("Closing due to inactivity");
      this->close();
    }
    else {
      resetRecentUsage();
      scheduleClosureWhenIdle();
    }
  });
  setExpirationTime(time::steady_clock::now() + m_idleTimeout);
}

void UnicastUdpTransport::handleAddressAdded(boost::asio::ip::address address)
{
  if (!m_hasAddress) {
    bool isTransportV6;
    getLocalUri().getScheme() == "udp4" ? isTransportV6 = false : isTransportV6 = true;
    changeSocketLocalAddress();
  }
}

void UnicastUdpTransport::handleAddressRemoved(boost::asio::ip::address address)
{
  if (m_socket.local_endpoint().address() == address) {
    m_hasAddress = false;
    changeSocketLocalAddress();

    if (!m_hasAddress) {
      /*if (m_socket.is_open()) { // TODO remove
        NFD_LOG_TRACE("loller");
        // Cancel all outstanding operations and close the socket.
        // Use the non-throwing variants and ignore errors, if any.
        boost::system::error_code error;
        m_socket.cancel(error);
        m_socket.close(error);
      }*/
    }
  }
}

void UnicastUdpTransport::changeSocketLocalAddress()
{
  bool isTransportV6;
  getLocalUri().getScheme() == "udp4" ? isTransportV6 = false : isTransportV6 = true;

  boost::asio::ip::address address;
  if (!isTransportV6) {
    for (const boost::asio::ip::address_v4& addr : m_networkInterface->getIpv4Addresses()) {
      if(!addr.is_loopback() && !addr.is_multicast())
        address = addr;
    }
  }
  else {
    for (const boost::asio::ip::address_v6& addr : m_networkInterface->getIpv6Addresses()) {
      if(!addr.is_loopback() && !addr.is_multicast() && !addr.is_multicast_link_local())
        address = addr;
    }
  }

  if (!address.is_unspecified() && !address.is_loopback()) {
    NFD_LOG_FACE_INFO("Changing local address to " << address);
    rebindSocket(udp::Endpoint(address, m_localEndpointPort));
    m_hasAddress = true;

#ifdef __linux__
    //
    // By default, Linux does path MTU discovery on IPv4 sockets,
    // and sets the DF (Don't Fragment) flag on datagrams smaller
    // than the interface MTU. However this does not work for us,
    // because we cannot properly respond to ICMP "packet too big"
    // messages by fragmenting the packet at the application level,
    // since we want to rely on IP for fragmentation and reassembly.
    //
    // Therefore, we disable PMTU discovery, which prevents the kernel
    // from setting the DF flag on outgoing datagrams, and thus allows
    // routers along the path to perform fragmentation as needed.
    //
    const int value = IP_PMTUDISC_DONT;
    if (::setsockopt(m_socket.native_handle(), IPPROTO_IP,
                     IP_MTU_DISCOVER, &value, sizeof(value)) < 0) {
      NFD_LOG_FACE_WARN("Failed to disable path MTU discovery: " << std::strerror(errno));
    }
#endif
  }
}

} // namespace face
} // namespace nfd
