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

#include "udp-factory.hpp"
#include "generic-link-service.hpp"
#include "unicast-udp-transport.hpp"
#include "multicast-udp-transport.hpp"
#include "core/global-io.hpp"
#include "core/global-network-monitor.hpp"

#ifdef __linux__
#include <cerrno>       // for errno
#include <cstring>      // for std::strerror()
#include <sys/socket.h> // for setsockopt()
#endif

namespace nfd {

namespace ip = boost::asio::ip;

NFD_LOG_INIT("UdpFactory");

void
UdpFactory::prohibitEndpoint(const udp::Endpoint& endpoint)
{
  if (endpoint.address().is_v4() &&
      endpoint.address() == ip::address_v4::any()) {
    prohibitAllIpv4Endpoints(endpoint.port());
  }
  else if (endpoint.address().is_v6() &&
           endpoint.address() == ip::address_v6::any()) {
    prohibitAllIpv6Endpoints(endpoint.port());
  }

  NFD_LOG_TRACE("prohibiting UDP " << endpoint);
  m_prohibitedEndpoints.insert(endpoint);
}

void
UdpFactory::prohibitAllIpv4Endpoints(uint16_t port)
{
  for (const shared_ptr<ndn::util::NetworkInterface>& nic : getGlobalNetworkMonitor().listNetworkInterfaces()) {
    for (const auto& addr : nic->getIpv4Addresses()) {
      if (addr != ip::address_v4::any()) {
        prohibitEndpoint(udp::Endpoint(addr, port));
      }
    }

    if (nic->isBroadcastCapable() &&
        nic->getIpv4BroadcastAddress() != ip::address_v4::any()) {
      prohibitEndpoint(udp::Endpoint(nic->getIpv4BroadcastAddress(), port));
    }
  }

  prohibitEndpoint(udp::Endpoint(ip::address_v4::broadcast(), port));
}

void
UdpFactory::prohibitAllIpv6Endpoints(uint16_t port)
{
  for (const shared_ptr<ndn::util::NetworkInterface>& nic : getGlobalNetworkMonitor().listNetworkInterfaces()) {
    for (const auto& addr : nic->getIpv6Addresses()) {
      if (addr != ip::address_v6::any()) {
        prohibitEndpoint(udp::Endpoint(addr, port));
      }
    }
  }
}

shared_ptr<UdpChannel>
UdpFactory::createChannel(const udp::Endpoint& endpoint,
                          const shared_ptr<ndn::util::NetworkInterface>& ni,
                          const time::seconds& timeout)
{
  NFD_LOG_DEBUG("Creating unicast channel " << endpoint);

  auto channel = findChannel(endpoint);
  if (channel)
    return channel;

  if (endpoint.address().is_multicast()) {
    BOOST_THROW_EXCEPTION(Error("createChannel is only for unicast channels. The provided endpoint "
                                "is multicast. Use createMulticastFace to create a multicast face"));
  }

  // check if the endpoint is already used by a multicast face
  auto face = findMulticastFace(endpoint);
  if (face) {
    BOOST_THROW_EXCEPTION(Error("Cannot create the requested UDP unicast channel, local "
                                "endpoint is already allocated for a UDP multicast face"));
  }

  channel = make_shared<UdpChannel>(endpoint, ni, timeout);
  m_channels[endpoint] = channel;
  prohibitEndpoint(endpoint);

  return channel;
}

shared_ptr<UdpChannel>
UdpFactory::createChannel(const std::string& localIp,
                          const std::string& localPort,
                          const shared_ptr<ndn::util::NetworkInterface>& ni,
                          const time::seconds& timeout)
{
  udp::Endpoint endpoint(ip::address::from_string(localIp),
                         boost::lexical_cast<uint16_t>(localPort));
  return createChannel(endpoint, ni, timeout);
}

shared_ptr<Face>
UdpFactory::createMulticastFace(const udp::Endpoint& localEndpoint,
                                const udp::Endpoint& multicastEndpoint,
                                const std::string& networkInterfaceName/* = ""*/)
{
  // checking if the local and multicast endpoints are already in use for a multicast face
  auto face = findMulticastFace(localEndpoint);
  if (face) {
    if (face->getRemoteUri() == FaceUri(multicastEndpoint))
      return face;
    else
      BOOST_THROW_EXCEPTION(Error("Cannot create the requested UDP multicast face, local "
                                  "endpoint is already allocated for a UDP multicast face "
                                  "on a different multicast group"));
  }

  // checking if the local endpoint is already in use for a unicast channel
  auto unicastCh = findChannel(localEndpoint);
  if (unicastCh) {
    BOOST_THROW_EXCEPTION(Error("Cannot create the requested UDP multicast face, local "
                                "endpoint is already allocated for a UDP unicast channel"));
  }

  if (m_prohibitedEndpoints.find(multicastEndpoint) != m_prohibitedEndpoints.end()) {
    BOOST_THROW_EXCEPTION(Error("Cannot create the requested UDP multicast face, "
                                "remote endpoint is owned by this NFD instance"));
  }

  if (localEndpoint.address().is_v6() || multicastEndpoint.address().is_v6()) {
    BOOST_THROW_EXCEPTION(Error("IPv6 multicast is not supported yet. Please provide an IPv4 "
                                "address"));
  }

  if (localEndpoint.port() != multicastEndpoint.port()) {
    BOOST_THROW_EXCEPTION(Error("Cannot create the requested UDP multicast face, "
                                "both endpoints should have the same port number. "));
  }

  if (!multicastEndpoint.address().is_multicast()) {
    BOOST_THROW_EXCEPTION(Error("Cannot create the requested UDP multicast face, "
                                "the multicast group given as input is not a multicast address"));
  }

  ip::udp::socket receiveSocket(getGlobalIoService());
  receiveSocket.open(multicastEndpoint.protocol());
  receiveSocket.set_option(ip::udp::socket::reuse_address(true));
  receiveSocket.bind(multicastEndpoint);

  ip::udp::socket sendSocket(getGlobalIoService());
  sendSocket.open(multicastEndpoint.protocol());
  sendSocket.set_option(ip::udp::socket::reuse_address(true));
  sendSocket.set_option(ip::multicast::enable_loopback(false));
  sendSocket.bind(udp::Endpoint(ip::address_v4::any(), multicastEndpoint.port()));
  if (localEndpoint.address() != ip::address_v4::any())
    sendSocket.set_option(ip::multicast::outbound_interface(localEndpoint.address().to_v4()));

  sendSocket.set_option(ip::multicast::join_group(multicastEndpoint.address().to_v4(),
                                                  localEndpoint.address().to_v4()));
  receiveSocket.set_option(ip::multicast::join_group(multicastEndpoint.address().to_v4(),
                                                     localEndpoint.address().to_v4()));

#ifdef __linux__
  /*
   * On Linux, if there is more than one MulticastUdpFace for the same multicast
   * group but they are bound to different network interfaces, the socket needs
   * to be bound to the specific interface using SO_BINDTODEVICE, otherwise the
   * face will receive all packets sent to the other interfaces as well.
   * This happens only on Linux. On OS X, the ip::multicast::join_group option
   * is enough to get the desired behaviour.
   */
  if (!networkInterfaceName.empty()) {
    if (::setsockopt(receiveSocket.native_handle(), SOL_SOCKET, SO_BINDTODEVICE,
                     networkInterfaceName.c_str(), networkInterfaceName.size() + 1) < 0) {
      BOOST_THROW_EXCEPTION(Error("Cannot bind multicast face to " + networkInterfaceName +
                                  ": " + std::strerror(errno)));
    }
  }
#endif

  auto linkService = make_unique<face::GenericLinkService>();
  auto transport = make_unique<face::MulticastUdpTransport>(localEndpoint, multicastEndpoint,
                                                            std::move(receiveSocket),
                                                            std::move(sendSocket));
  face = make_shared<Face>(std::move(linkService), std::move(transport));

  m_multicastFaces[localEndpoint] = face;
  connectFaceClosedSignal(*face, [this, localEndpoint] { m_multicastFaces.erase(localEndpoint); });

  return face;
}

shared_ptr<Face>
UdpFactory::createMulticastFace(const std::string& localIp,
                                const std::string& multicastIp,
                                const std::string& multicastPort,
                                const std::string& networkInterfaceName/* = ""*/)
{
  udp::Endpoint localEndpoint(ip::address::from_string(localIp),
                              boost::lexical_cast<uint16_t>(multicastPort));
  udp::Endpoint multicastEndpoint(ip::address::from_string(multicastIp),
                                  boost::lexical_cast<uint16_t>(multicastPort));
  return createMulticastFace(localEndpoint, multicastEndpoint, networkInterfaceName);
}

shared_ptr<nfd::face::Face> UdpFactory::createInterfaceFace(short localEndpointPort,
                                                            const udp::Endpoint& remoteEndpoint,
                                                            const shared_ptr<ndn::util::NetworkInterface>& ni)
{
  // checking if the local and multicast endpoints are already in use for a multicast face
  auto face = findInterfaceFace(ni->getName(), remoteEndpoint);
  if (face)
    return face;

  // checking if the local endpoint is already in use for a unicast channel
  /*auto unicastCh = findChannel(localEndpoint);
  if (unicastCh) {
    BOOST_THROW_EXCEPTION(Error("Cannot create the requested UDP interface face, local "
                                "endpoint is already allocated for a UDP unicast channel"));
  }

  if (m_prohibitedEndpoints.find(localEndpoint) != m_prohibitedEndpoints.end()) {
    BOOST_THROW_EXCEPTION(Error("Cannot create the requested UDP interface face, "
                                "local endpoint is owned by this NFD instance"));
  }

  if (localEndpoint.address().is_multicast()) {
    BOOST_THROW_EXCEPTION(Error("createInterfaceFace is only for unicast channels. The provided local "
                                "endpoint is multicast. Use createMulticastFace to create a multicast face"));
  }

  if (remoteEndpoint.address().is_multicast()) {
    BOOST_THROW_EXCEPTION(Error("createInterfaceFace is only for unicast channels. The provided remote "
                                "endpoint is multicast. Use createMulticastFace to create a multicast face"));
  }*/

  /*if (localEndpoint.address().is_v6() || remoteEndpoint.address().is_v6()) {
    BOOST_THROW_EXCEPTION(Error("IPv6 multicast is not supported yet. Please provide an IPv4 "
                                "address"));
  }*/

  /*if (localEndpoint.port() != remoteEndpoint.port()) {
    BOOST_THROW_EXCEPTION(Error("Cannot create the requested UDP multicast face, "
                                "both endpoints should have the same port number. "));
  }*/ // TODO Why?

  /*ip::udp::socket socket(getGlobalIoService(), remoteEndpoint.protocol());
  socket.set_option(ip::udp::socket::reuse_address(true));
  NFD_LOG_TRACE("Local endpoint bind " << localEndpoint.address().to_string());
  socket.bind(localEndpoint);
  socket.connect(remoteEndpoint);*/

  auto linkService = make_unique<face::GenericLinkService>();
  //TODO mio auto
  unique_ptr<face::UnicastUdpTransport> transport =
      make_unique<face::UnicastUdpTransport>(localEndpointPort, remoteEndpoint, ni);
  face = make_shared<Face>(std::move(linkService), std::move(transport));

  auto& faces = m_interfaceFaces[ni->getName()];
  faces[remoteEndpoint] = face;
  connectFaceClosedSignal(*face, [this, ni] { m_interfaceFaces.erase(ni->getName()); });
  //prohibitEndpoint(localEndpoint); //TODO check

  return face;
}

void
UdpFactory::createFace(const FaceUri& uri,
                       ndn::nfd::FacePersistency persistency,
                       const FaceCreatedCallback& onCreated,
                       const FaceCreationFailedCallback& onConnectFailed)
{
  BOOST_ASSERT(uri.isCanonical());

  if (persistency == ndn::nfd::FACE_PERSISTENCY_ON_DEMAND) {
    BOOST_THROW_EXCEPTION(Error("UdpFactory::createFace does not support FACE_PERSISTENCY_ON_DEMAND"));
  }

  udp::Endpoint endpoint(ip::address::from_string(uri.getHost()),
                         boost::lexical_cast<uint16_t>(uri.getPort()));

  if (endpoint.address().is_multicast()) {
    onConnectFailed("The provided address is multicast. Please use createMulticastFace method");
    return;
  }

  if (m_prohibitedEndpoints.find(endpoint) != m_prohibitedEndpoints.end()) {
    onConnectFailed("Requested endpoint is prohibited "
                    "(reserved by this NFD or disallowed by face management protocol)");
    return;
  }

  // very simple logic for now
  for (const auto& i : m_channels) {
    if ((i.first.address().is_v4() && endpoint.address().is_v4()) ||
        (i.first.address().is_v6() && endpoint.address().is_v6())) {
      i.second->connect(endpoint, persistency, onCreated, onConnectFailed);
      return;
    }
  }

  onConnectFailed("No channels available to connect to " + boost::lexical_cast<std::string>(endpoint));
}

void UdpFactory::createFace(const ndn::util::FaceUri& uri,
                            const ndn::util::FaceUri& localUri,
                            ndn::nfd::FacePersistency persistency,
                            const FaceCreatedCallback& onCreated,
                            const FaceCreationFailedCallback& onConnectFailed)
{
  BOOST_ASSERT(uri.isCanonical());

  if (persistency == ndn::nfd::FACE_PERSISTENCY_ON_DEMAND) {
    BOOST_THROW_EXCEPTION(Error("UdpFactory::createFace does not support FACE_PERSISTENCY_ON_DEMAND"));
  }

  udp::Endpoint endpoint(ip::address::from_string(uri.getHost()),
                         boost::lexical_cast<uint16_t>(uri.getPort()));

  if (endpoint.address().is_multicast()) {
    onConnectFailed("The provided address is multicast. Please use createMulticastFace method");
    return;
  }

  if (m_prohibitedEndpoints.find(endpoint) != m_prohibitedEndpoints.end()) {
    onConnectFailed("Requested endpoint is prohibited "
                    "(reserved by this NFD or disallowed by face management protocol)");
    return;
  }

  NFD_LOG_DEBUG("*** Chosing channels: " << uri << "  " << persistency);

  udp::Endpoint localEndpoint(ip::address::from_string(localUri.getHost()),
                              boost::lexical_cast<uint16_t>(localUri.getPort()));

  auto channel = m_channels.find(localEndpoint);
  if (channel != m_channels.end()) {
    channel->second->connect(endpoint, persistency, onCreated, onConnectFailed);
    return;
  }

  onConnectFailed("No channels with the correspoding address: " + boost::lexical_cast<std::string>(localEndpoint));
}

std::vector<shared_ptr<const Channel>>
UdpFactory::getChannels() const
{
  std::vector<shared_ptr<const Channel>> channels;
  channels.reserve(m_channels.size());

  for (const auto& i : m_channels)
    channels.push_back(i.second);

  return channels;
}

shared_ptr<UdpChannel>
UdpFactory::findChannel(const udp::Endpoint& localEndpoint) const
{
  auto i = m_channels.find(localEndpoint);
  if (i != m_channels.end())
    return i->second;
  else
    return nullptr;
}

shared_ptr<Face>
UdpFactory::findMulticastFace(const udp::Endpoint& localEndpoint) const
{
  auto i = m_multicastFaces.find(localEndpoint);
  if (i != m_multicastFaces.end())
    return i->second;

  return nullptr;
}

shared_ptr<nfd::face::Face>
UdpFactory::findInterfaceFace(const std::string& interfaceName,
                              const udp::Endpoint& remoteEndpoint) const
{
  auto i = m_interfaceFaces.find(interfaceName);
  if (i != m_interfaceFaces.end()) {
    auto j = i->second.find(remoteEndpoint);
    if (j != i->second.end())
      return j->second;
  }

  return nullptr;
}

} // namespace nfd
