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

#ifndef NFD_DAEMON_MGMT_FACE_MANAGER_HPP
#define NFD_DAEMON_MGMT_FACE_MANAGER_HPP

#include "manager-base.hpp"
#include <ndn-cxx/management/nfd-face-status.hpp>
#include <ndn-cxx/management/nfd-face-query-filter.hpp>
#include "face/face.hpp"

#include <ndn-cxx/util/network-monitor.hpp> //TODO mio forward decl?
#include <ndn-cxx/util/network-interface.hpp> //TODO mio forward decl?

#ifdef HAVE_LIBPCAP // TODO mio classe ethernet forward?
#include "face/ethernet-factory.hpp"
#include "face/ethernet-transport.hpp"
#endif // HAVE_LIBPCAP

namespace nfd {

class FaceTable;
class NetworkInterfaceInfo;
class ProtocolFactory;

// ; the unix section contains settings of Unix stream faces and channels
// unix
// {
//   path /var/run/nfd.sock ; Unix stream listener path
// }
struct UnixConfig {
  std::string path = "/var/run/nfd.sock";
};

// ; the tcp section contains settings of TCP faces and channels
// tcp
// {
//   listen yes ; set to 'no' to disable TCP listener, default 'yes'
//   port 6363 ; TCP listener port number
// }
struct TcpConfig {
  uint16_t port = 6363;
  bool needToListen = true;
  bool enableV4 = true;
  bool enableV6 = true;
};

// ; the udp section contains settings of UDP faces and channels
// udp
// {
//   port 6363 ; UDP unicast port number
//   idle_timeout 600 ; idle time (seconds) before closing a UDP unicast face
//   keep_alive_interval 25 ; interval (seconds) between keep-alive refreshes

//   ; NFD creates one UDP multicast face per NIC
//   mcast yes ; set to 'no' to disable UDP multicast, default 'yes'
//   mcast_port 56363 ; UDP multicast port number
//   mcast_group 224.0.23.170 ; UDP multicast group (IPv4 only)
// }
struct UdpConfig {
  uint16_t port = 6262; // TODO mio 6363
  bool enableV4 = true;
  bool enableV6 = true;
  size_t timeout = 600;
  size_t keepAliveInterval = 25;
  bool useMcast = true;
  boost::asio::ip::address_v4 mcastGroup = boost::asio::ip::address_v4::from_string("224.0.23.170");
  uint16_t mcastPort = 56363;
};

// ; the ether section contains settings of Ethernet faces and channels
// ether
// {
//   ; NFD creates one Ethernet multicast face per NIC
//   mcast yes ; set to 'no' to disable Ethernet multicast, default 'yes'
//   mcast_group 01:00:5E:00:17:AA ; Ethernet multicast group
// }

struct EtherConfig {
  #ifdef HAVE_LIBPCAP
    bool useMcast = true;
    ethernet::Address mcastGroup = ethernet::getDefaultMulticastAddress();
  #endif // HAVE_LIBPCAP
};

// ; the websocket section contains settings of WebSocket faces and channels
// websocket
// {
//   listen yes ; set to 'no' to disable WebSocket listener, default 'yes'
//   port 9696 ; WebSocket listener port number
//   enable_v4 yes ; set to 'no' to disable listening on IPv4 socket, default 'yes'
//   enable_v6 yes ; set to 'no' to disable listening on IPv6 socket, default 'yes'
// }

struct WebSocketConfig {
  uint16_t port = 9696;
  bool needToListen = true;
  bool enableV4 = true;
  bool enableV6 = true;
};

/**
 * @brief implement the Face Management of NFD Management Protocol.
 * @sa http://redmine.named-data.net/projects/nfd/wiki/FaceMgmt
 */
class FaceManager : public ManagerBase
{
public:
  FaceManager(FaceTable& faceTable,
              Dispatcher& dispatcher,
              CommandValidator& validator);

  /**
   * @brief Subscribe to face_system section for the config file
   */
  void
  setConfigFile(ConfigFile& configFile);

PUBLIC_WITH_TESTS_ELSE_PRIVATE: // ControlCommand
  void
  createFace(const Name& topPrefix, const Interest& interest,
             const ControlParameters& parameters,
             const ndn::mgmt::CommandContinuation& done);

  void
  destroyFace(const Name& topPrefix, const Interest& interest,
              const ControlParameters& parameters,
              const ndn::mgmt::CommandContinuation& done);

  void
  enableLocalControl(const Name& topPrefix, const Interest& interest,
                     const ControlParameters& parameters,
                     const ndn::mgmt::CommandContinuation& done);

  void
  disableLocalControl(const Name& topPrefix, const Interest& interest,
                      const ControlParameters& parameters,
                      const ndn::mgmt::CommandContinuation& done);

PUBLIC_WITH_TESTS_ELSE_PRIVATE: // helpers for ControlCommand
  void
  afterCreateFaceSuccess(ControlParameters& parameters,
                         const shared_ptr<Face>& newFace,
                         const ndn::mgmt::CommandContinuation& done);

  void
  afterCreateFaceFailure(const std::string& reason,
                         const ndn::mgmt::CommandContinuation& done);

  Face*
  findFaceForLocalControl(const Interest& request,
                          const ControlParameters& parameters,
                          const ndn::mgmt::CommandContinuation& done);

PUBLIC_WITH_TESTS_ELSE_PRIVATE: // StatusDataset
  void
  listFaces(const Name& topPrefix, const Interest& interest,
            ndn::mgmt::StatusDatasetContext& context);

  void
  listChannels(const Name& topPrefix, const Interest& interest,
               ndn::mgmt::StatusDatasetContext& context);

  void
  queryFaces(const Name& topPrefix, const Interest& interest,
             ndn::mgmt::StatusDatasetContext& context);

private: // helpers for StatusDataset handler
  bool
  doesMatchFilter(const ndn::nfd::FaceQueryFilter& filter, shared_ptr<Face> face);

  /** \brief get status of face, including properties and counters
   */
  static ndn::nfd::FaceStatus
  collectFaceStatus(const Face& face, const time::steady_clock::TimePoint& now);

  /** \brief copy face properties into traits
   *  \tparam FaceTraits either FaceStatus or FaceEventNotification
   */
  template<typename FaceTraits>
  static void
  collectFaceProperties(const Face& face, FaceTraits& traits);

private: // NotificationStream
  void
  afterFaceAdded(shared_ptr<Face> face,
                 const ndn::mgmt::PostNotification& post);

  void
  afterFaceRemoved(shared_ptr<Face> face,
                   const ndn::mgmt::PostNotification& post);

private: // configuration
  void
  processConfig(const ConfigSection& configSection, bool isDryRun,
                const std::string& filename);

  void
  processSectionUnix(const ConfigSection& configSection, bool isDryRun);

  void
  processSectionTcp(const ConfigSection& configSection, bool isDryRun);

  void
  processSectionUdp(const ConfigSection& configSection, bool isDryRun,
                    const std::vector<shared_ptr<ndn::util::NetworkInterface>>& nicList);

  void
  processSectionEther(const ConfigSection& configSection, bool isDryRun,
                      const std::vector<shared_ptr<ndn::util::NetworkInterface>>& nicList);

  void
  processSectionWebSocket(const ConfigSection& configSection, bool isDryRun);

  void
  handleInterfaceAdded(const shared_ptr<ndn::util::NetworkInterface>& ni);

  void
  handleInterfaceStateChanged(const shared_ptr<ndn::util::NetworkInterface>& ni,
                              ndn::util::NetworkInterfaceState oldState,
                              ndn::util::NetworkInterfaceState newState);
  void
  handleInterfaceRemoved(const shared_ptr<ndn::util::NetworkInterface>& ni);

  void
  handleInterfaceAddressAdded(const shared_ptr<ndn::util::NetworkInterface>& ni,
                              boost::asio::ip::address address);

  void
  handleInterfaceAddressRemoved(const shared_ptr<ndn::util::NetworkInterface>& ni,
                                boost::asio::ip::address address);

PUBLIC_WITH_TESTS_ELSE_PRIVATE:
  std::map<std::string /*protocol*/, shared_ptr<ProtocolFactory>> m_factories;

private:
  FaceTable& m_faceTable;
  signal::ScopedConnection m_faceAddConn;
  signal::ScopedConnection m_faceRemoveConn;

private:  // Config
  UnixConfig m_unixConfig;
  TcpConfig m_tcpConfig;
  UdpConfig m_udpConfig;
  EtherConfig m_etherConfig;
  WebSocketConfig m_webSocketConfig;
};

} // namespace nfd

#endif // NFD_DAEMON_MGMT_FACE_MANAGER_HPP
