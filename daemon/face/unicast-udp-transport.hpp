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

#ifndef NFD_DAEMON_FACE_UNICAST_UDP_TRANSPORT_HPP
#define NFD_DAEMON_FACE_UNICAST_UDP_TRANSPORT_HPP

#include "datagram-transport.hpp"
#include "core/scheduler.hpp"

#include <ndn-cxx/util/network-interface.hpp> // TODO mio forward decl

namespace nfd {
namespace face {

/**
 * \brief A Transport that communicates on a unicast UDP socket
 */
class UnicastUdpTransport DECL_CLASS_FINAL : public DatagramTransport<boost::asio::ip::udp, Unicast>
{
public:
  UnicastUdpTransport(protocol::socket&& socket,
                      ndn::nfd::FacePersistency persistency,
                      time::nanoseconds idleTimeout,
                      const shared_ptr<ndn::util::NetworkInterface>& ni);

  UnicastUdpTransport(short localEndpointPort,
                      udp::Endpoint remoteEndpoint,
                      const shared_ptr<ndn::util::NetworkInterface>& ni);

  virtual std::string
  getInterfaceName() const DECL_FINAL;

protected:
  virtual void
  beforeChangePersistency(ndn::nfd::FacePersistency newPersistency) DECL_FINAL;

  void
  changeStateFromInterface(ndn::util::NetworkInterfaceState state);

private:
  void
  scheduleClosureWhenIdle();

  void
  handleAddressAdded(boost::asio::ip::address address);

  void
  handleAddressRemoved(boost::asio::ip::address address);

  void
  changeSocketLocalAddress();

private:
  const time::nanoseconds m_idleTimeout;
  scheduler::ScopedEventId m_closeIfIdleEvent;
  shared_ptr<ndn::util::NetworkInterface> m_networkInterface;
  bool m_hasAddress;
  short m_localEndpointPort;
};

inline std::string
UnicastUdpTransport::getInterfaceName() const
{
  return m_networkInterface->getName();
}

} // namespace face
} // namespace nfd

#endif // NFD_DAEMON_FACE_UNICAST_UDP_TRANSPORT_HPP
