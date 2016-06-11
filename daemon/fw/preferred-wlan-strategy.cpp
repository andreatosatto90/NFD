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

#include "preferred-wlan-strategy.hpp"
#include "core/logger.hpp"
#include "strategies-tracepoint.hpp"
#include "core/global-io.hpp"
#include "core/global-network-monitor.hpp"
#include <thread> //TODO test only


namespace nfd {
namespace fw {

NFD_LOG_INIT("PreferredWlanStrategy");

const Name PreferredWlanStrategy::STRATEGY_NAME("ndn:/localhost/nfd/strategy/preferred-wlan/%FD%01");
NFD_REGISTER_STRATEGY(PreferredWlanStrategy);

PreferredWlanStrategy::PreferredWlanStrategy(Forwarder& forwarder, const Name& name)
  : RetriesStrategy(forwarder, name)
{
  std::random_device rd;
  m_randomGen.seed(rd());


  // Set weight 2 to preferred interface, 1 to the secondary, 0 to unused
  m_interfacesInfo.insert(std::make_pair("eth0",InterfaceInfo("eth0", 2)));
  //m_interfacesInfo.insert(std::make_pair("enp2s0",InterfaceInfo("enp2s0", 0)));
  m_interfacesInfo.insert(std::make_pair("wwan0",InterfaceInfo("wwan0", 0)));
  m_interfacesInfo.insert(std::make_pair("wlan0",InterfaceInfo("wlan0", 1)));
  m_interfacesInfo.insert(std::make_pair("wlp4s0",InterfaceInfo("wlp4s0", 0)));
}

PreferredWlanStrategy::~PreferredWlanStrategy()
{
}

/** \brief determines whether a NextHop is eligible
 *  \param pitEntry PIT entry
 *  \param nexthop next hop
 *  \param currentDownstream incoming FaceId of current Interest
 *  \param weight interface weight
 *  \param selWeight the interface needs the specified weight to be eligible
 */
static inline bool
predicate_NextHop_eligible(const shared_ptr<pit::Entry>& pitEntry,
                           const fib::NextHop& nexthop,
                           FaceId currentDownstream,
                           int weight = 0,
                           int selWeight = 0)
{
  shared_ptr<Face> upstream = nexthop.getFace();

  // upstream is current downstream
  if (upstream->getId() == currentDownstream)
    return false;

  // forwarding would violate scope
  if (pitEntry->violatesScope(*upstream))
    return false;

  if (upstream->getState() == nfd::face::TransportState::DOWN)
    return false;

  if (weight != selWeight)
    return false;

  return true;
}

void
PreferredWlanStrategy::afterReceiveInterest(const Face& inFace,
                                            const Interest& interest,
                                            shared_ptr<fib::Entry> fibEntry,
                                            shared_ptr<pit::Entry> pitEntry)
{
  float totalWeight = 0;
  const fib::NextHopList& nexthops = fibEntry->getNextHops();
  std::map<int, shared_ptr<Face>> eligibleFaces;

  // get face with weight == 1
  for (const fib::NextHop& nextHop : nexthops) {
    if (predicate_NextHop_eligible(pitEntry, nextHop, inFace.getId(), 2, getFaceWeight(nextHop.getFace()))) {
      shared_ptr<Face> outFace = nextHop.getFace();
      int prob = getFaceWeight(outFace);
      if (prob > 0) {

        totalWeight += prob;
        eligibleFaces[totalWeight] = outFace;
        //NFD_LOG_DEBUG("Eligible face: " << outFace->getId());
      }
    }
  }

  // if no face with weight 1 are eligible get face with weight == 0
  if (eligibleFaces.size() == 0) {
    for (const fib::NextHop& nextHop : nexthops) {
      if (predicate_NextHop_eligible(pitEntry, nextHop, inFace.getId(), 1, getFaceWeight(nextHop.getFace()))) {
        shared_ptr<Face> outFace = nextHop.getFace();
        int prob = getFaceWeight(outFace);
        if (prob > 0) {

          totalWeight += prob;
          eligibleFaces[totalWeight] = outFace;
          //NFD_LOG_DEBUG("Eligible face: " << outFace->getId());
        }
      }
    }
  }

  if (eligibleFaces.size() > 0) {
    std::uniform_int_distribution<> dis(1, totalWeight);
    int randomValue = dis(m_randomGen);

    auto it = std::find_if(eligibleFaces.begin(), eligibleFaces.end(),
                            [randomValue] (const std::pair<const int,shared_ptr<Face>>& hop) {
                                             return randomValue <= hop.first;
                                           });

    if (it != eligibleFaces.end()) {
      shared_ptr<Face> outFace = it->second;
      NFD_LOG_DEBUG("Interest to interface: " << outFace->getInterfaceName());
      //this->sendInterest(pitEntry, outFace);


      insertPendingInterest(interest, outFace, fibEntry, pitEntry); // Also send the interest TODO change name

      /*tracepoint(strategyLog, interest_sent, STRATEGY_NAME.toUri().c_str(), interest.toUri().c_str(),
                 outFace->getId(), outFace->getInterfaceName().c_str(), rttEstimators[outFace->getInterfaceName()].computeRto());*/
      return;
    }
    else
      NFD_LOG_TRACE("No eligible faces 1");
  }
  else
    NFD_LOG_TRACE("No eligible faces 2");

  NFD_LOG_TRACE("No eligible faces, waiting to send");

  insertPendingInterest(interest, nullptr, fibEntry, pitEntry);


  /*NFD_LOG_TRACE("Interest rejected");

  lp::NackHeader nackHeader;
  nackHeader.setReason(lp::NackReason::DUPLICATE);
  this->sendNack(pitEntry, inFace, nackHeader);
  this->rejectPendingInterest(pitEntry);*/
  return;
}

int
PreferredWlanStrategy::getFaceWeight(const shared_ptr<Face>& face) const
{
  std::string nicName = face->getInterfaceName();
  auto it = m_interfacesInfo.find(nicName);
  if (it != m_interfacesInfo.end())
    return it->second.weight;

  return 0;
}

bool
PreferredWlanStrategy::isMainInterface(std::string interfaceName)
{
  // return true if weight is 2
  auto it = m_interfacesInfo.find(interfaceName);
  if (it != m_interfacesInfo.end() && it->second.weight == 2)
    return true;

  return false;
}

} // namespace fw
} // namespace nfd
