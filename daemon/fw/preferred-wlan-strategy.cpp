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


namespace nfd {
namespace fw {

NFD_LOG_INIT("PreferredWlanStrategy");

const Name PreferredWlanStrategy::STRATEGY_NAME("ndn:/localhost/nfd/strategy/preferred-wlan/%FD%01");
NFD_REGISTER_STRATEGY(PreferredWlanStrategy);

PreferredWlanStrategy::PreferredWlanStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder, name)
{
  std::random_device rd;
  m_randomGen.seed(rd());

  // Set weight 1 to preferred interface, 0 to the other
  m_interfacesInfo.insert(std::make_pair("eth0",InterfaceInfo("eth0", 0)));
  m_interfacesInfo.insert(std::make_pair("wlan0",InterfaceInfo("wlan0", 1)));
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
 *  \param now time::steady_clock::now(), ignored if !wantUnused
 */
static inline bool
predicate_NextHop_eligible(const shared_ptr<pit::Entry>& pitEntry,
                           const fib::NextHop& nexthop, FaceId currentDownstream,
                           int weight = 0,
                           int selWeight = 0,
                           time::steady_clock::TimePoint now = time::steady_clock::TimePoint::min())
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
  const fib::NextHopList& nexthops = fibEntry->getNextHops();
  std::vector<shared_ptr<Face>> eligibleFaces;

  // get face with weight == 1
  for (const fib::NextHop& nextHop : nexthops) {
    if (predicate_NextHop_eligible(pitEntry, nextHop, inFace.getId(), 1,
                                   getFaceWeight(nextHop.getFace()), time::steady_clock::now())) {
      shared_ptr<Face> outFace = nextHop.getFace();
      eligibleFaces.push_back(outFace);
      tracepoint(strategyLog, interest_sent, STRATEGY_NAME.toUri().c_str(), interest.toUri().c_str(),
                 outFace->getId(), outFace->getTransport()->getInterfaceName().c_str());
    }
  }

  // if no face with weight 1 are eligible get face with weight == 0
  if (eligibleFaces.size() == 0) {
    for (const fib::NextHop& nextHop : nexthops) {
      if (predicate_NextHop_eligible(pitEntry, nextHop, inFace.getId(), 0,
                                     getFaceWeight(nextHop.getFace()),time::steady_clock::now())) {
        shared_ptr<Face> outFace = nextHop.getFace();
        eligibleFaces.push_back(outFace);
        tracepoint(strategyLog, interest_sent, STRATEGY_NAME.toUri().c_str(), interest.toUri().c_str(),
                   outFace->getId(), outFace->getInterfaceName().c_str());
      }
    }
  }

  if (eligibleFaces.size() > 0) {
    shared_ptr<Face> outFace = eligibleFaces[0];
    NFD_LOG_TRACE("Interest to face: " << outFace->getId());
    this->sendInterest(pitEntry, outFace);
    return;
  }

  NFD_LOG_TRACE("No eligible faces Interest rejected");

  this->rejectPendingInterest(pitEntry);
  return;
}

void
PreferredWlanStrategy::beforeSatisfyInterest(shared_ptr<pit::Entry> pitEntry,
                                              const nfd::face::Face& inFace,
                                              const ndn::Data& data)
{
  if (pitEntry->getOutRecords().size() > 0) // TODO we need the check?
    tracepoint(strategyLog, data_received, STRATEGY_NAME.toUri().c_str(), pitEntry->getInterest().toUri().c_str(),
               inFace.getId(), inFace.getInterfaceName().c_str());
}

int
PreferredWlanStrategy::getFaceWeight(const shared_ptr<Face>& face) const
{
  std::string nicName = face->getTransport()->getInterfaceName();
  auto it = m_interfacesInfo.find(nicName);
  if (it != m_interfacesInfo.end())
    return it->second.weight;
  else
    return 0;
}

} // namespace fw
} // namespace nfd
