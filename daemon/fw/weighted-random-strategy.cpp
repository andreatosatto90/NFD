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

#include "weighted-random-strategy.hpp"
#include "core/logger.hpp"
#include "strategies-tracepoint.hpp"


namespace nfd {
namespace fw {

NFD_LOG_INIT("WeightedRandomStrategy");

WeightedRandomStrategy::WeightedRandomStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder, name)
  , m_name(name)
{
  std::random_device rd;
  m_randomGen.seed(rd());
}

WeightedRandomStrategy::~WeightedRandomStrategy()
{
}

/** \brief determines whether a NextHop is eligible
 *  \param pitEntry PIT entry
 *  \param nexthop next hop
 *  \param currentDownstream incoming FaceId of current Interest
 *  \param checkState if true, face with transport state dwon will not be considered
 *  \param now time::steady_clock::now(), ignored if !wantUnused
 */
static inline bool
predicate_NextHop_eligible(const shared_ptr<pit::Entry>& pitEntry,
                           const fib::NextHop& nexthop, FaceId currentDownstream,
                           bool checkState = false,
                           time::steady_clock::TimePoint now = time::steady_clock::TimePoint::min())
{
  shared_ptr<Face> upstream = nexthop.getFace();

  // upstream is current downstream
  if (upstream->getId() == currentDownstream)
    return false;

  // forwarding would violate scope
  if (pitEntry->violatesScope(*upstream))
    return false;

  if (checkState && upstream->getState() == nfd::face::TransportState::DOWN)
    return false;

  return true;
}

void
WeightedRandomStrategy::afterReceiveInterest(const Face& inFace,
                                             const Interest& interest,
                                             shared_ptr<fib::Entry> fibEntry,
                                             shared_ptr<pit::Entry> pitEntry)
{
//  if (pitEntry->hasUnexpiredOutRecords()) {
//    // not a new Interest, don't forward
//    return;
//  }
  float totalWeight = 0;
  const fib::NextHopList& nexthops = fibEntry->getNextHops();
  std::map<int,shared_ptr<Face>> eligibleFaces;
  for (const fib::NextHop& nextHop : nexthops) {
    if (predicate_NextHop_eligible(pitEntry, nextHop, inFace.getId(), true, time::steady_clock::now())) {
      shared_ptr<Face> outFace = nextHop.getFace();
      int prob = getFaceWeight(outFace);
      if (prob > 0) {
        totalWeight += prob;
        eligibleFaces[totalWeight] = outFace;
        tracepoint(strategyLog, weighted_random, outFace->getId(), m_name.toUri().c_str());
        //NFD_LOG_TRACE("Eligible face: " << outFace->getId());
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
      NFD_LOG_TRACE("Interest to face: " << outFace->getId());
      this->sendInterest(pitEntry, outFace);
      return;
    }
  }
  else
    NFD_LOG_TRACE("No eligible faces");


  NFD_LOG_TRACE("Interest rejected");

  this->rejectPendingInterest(pitEntry);
  return;
}

int
WeightedRandomStrategy::getFaceWeight(const shared_ptr<Face>& face) const
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
