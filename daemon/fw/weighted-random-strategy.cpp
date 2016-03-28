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
#include "core/global-io.hpp"
#include "core/global-network-monitor.hpp"
#include <thread> //TODO test only


namespace nfd {
namespace fw {

NFD_LOG_INIT("WeightedRandomStrategy");

WeightedRandomStrategy::WeightedRandomStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder, name)
  , m_scheduler(getGlobalIoService())
  , m_name(name)
  , m_errorState(false)
  , m_runningInterface()
  , m_interfaceInterests()
{
  std::random_device rd;
  m_randomGen.seed(rd());

  getGlobalNetworkMonitor().onInterfaceAdded.connect(bind(&WeightedRandomStrategy::handleInterfaceAdded, this, _1));
  getGlobalNetworkMonitor().onInterfaceRemoved.connect(bind(&WeightedRandomStrategy::handleInterfaceRemoved, this, _1));

  rttMeanWeight.first = 0.2;  // Old value
  rttMeanWeight.second = 0.8; // New value
  m_rttMean = -1;
  m_lastRtt = -1;
  m_rttMulti = 2;
  m_rttMax = 1000;
  m_rtt0 = 700;

  m_nRttMean = 5;

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

  //if (retryEvent == nullptr )
    //retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(100), bind(&WeightedRandomStrategy::resendPendingInterestRetry, this)));
//  if (pitEntry->hasUnexpiredOutRecords()) {
//    // not a new Interest, don't forward
//    return;
//  }
  float totalWeight = 0;
  const fib::NextHopList& nexthops = fibEntry->getNextHops();
  std::map<int, shared_ptr<Face>> eligibleFaces;
  for (const fib::NextHop& nextHop : nexthops) {
    if (predicate_NextHop_eligible(pitEntry, nextHop, inFace.getId(), true, time::steady_clock::now())) {
      shared_ptr<Face> outFace = nextHop.getFace();
      int prob = getFaceWeight(outFace);
      if (prob > 0) {

        totalWeight += prob;
        eligibleFaces[totalWeight] = outFace;
        tracepoint(strategyLog, interest_sent, m_name.toUri().c_str(), interest.toUri().c_str(),
                   outFace->getId(), outFace->getInterfaceName().c_str());
        //NFD_LOG_DEBUG("Eligible face: " << outFace->getId());
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

      auto interestList = m_interfaceInterests.insert({outFace->getInterfaceName(), pendingInterests()}).first;

      auto pi = make_shared<PendingInterest>(PendingInterest(outFace->getInterfaceName(), outFace, interest, fibEntry, pitEntry, time::steady_clock::now(), nullptr));
      interestList->second.push_back(pi);
      pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())), bind(&WeightedRandomStrategy::retryInterest, this, pitEntry, outFace, time::steady_clock::now(), pi, false)));


      //NFD_LOG_DEBUG("Current pipeline size " << interestList->second.size());
      //
      lastFace = outFace;

      //NFD_LOG_TRACE("Size insert I: " << m_interfaceInterests[outFace->getInterfaceName()].size());
      return;
    }
    else
      NFD_LOG_TRACE("No eligible faces 1");
  }
  else
    NFD_LOG_TRACE("No eligible faces 2");

  NFD_LOG_TRACE("Interest rejected x");

  /*lp::NackHeader nackHeader;
  nackHeader.setReason(lp::NackReason::CONGESTION);
  this->sendNack(pitEntry, inFace, nackHeader);*/

  //auto interestList = m_interfaceInterests.insert({lastFace->getInterfaceName(), pendingInterests()}).first;
  //interestList->second.push_back(PendingInterest(lastFace->getInterfaceName(), lastFace, interest, fibEntry, pitEntry));
  //this->rejectPendingInterest(pitEntry);

  auto interestList = m_interfaceInterests.insert({lastFace->getInterfaceName(), pendingInterests()}).first;

  auto pi = make_shared<PendingInterest>(PendingInterest(lastFace->getInterfaceName(), lastFace, interest, fibEntry, pitEntry, time::steady_clock::now(), nullptr));
  interestList->second.push_back(pi);
  pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())), bind(&WeightedRandomStrategy::retryInterest, this, pitEntry, lastFace, time::steady_clock::now(), pi, false)));


  return;
}

void
WeightedRandomStrategy::beforeSatisfyInterest(shared_ptr<pit::Entry> pitEntry,
                                              const nfd::face::Face& inFace,
                                              const ndn::Data& data)
{
  time::milliseconds rtt = time::milliseconds(-1);
  pit::OutRecordCollection::const_iterator outRecord = pitEntry->getOutRecord(inFace);
  if (outRecord == pitEntry->getOutRecords().end()) { // no OutRecord
    /*NFD_LOG_TRACE(pitEntry->getInterest() << " dataFrom " << inFace.getId() <<
                  " no-out-record");*/
  }
  else {
    rtt = time::duration_cast<time::milliseconds> (time::steady_clock::now() - outRecord->getLastRenewed());
    addRttMeasurement(rtt.count());
    //NFD_LOG_DEBUG("Mean Rtt: " << m_rttMean);
  }

  if (pitEntry->getOutRecords().size() > 0) // TODO we need the check?
    tracepoint(strategyLog, data_received, m_name.toUri().c_str(), pitEntry->getInterest().toUri().c_str(),
               inFace.getId(), inFace.getInterfaceName().c_str(), rtt.count(), m_rttMean);

  for (auto& el : m_interfaceInterests) {
    //NFD_LOG_DEBUG("Current pipeline B " << el.second.size());
    el.second.erase(std::remove_if(el.second.begin(), el.second.end(),
                      [&](shared_ptr<PendingInterest>& pi) {

                          if((pi->pitEntry.get() == pitEntry.get()) || !pi->pitEntry->hasUnexpiredOutRecords()) {
                              if (pi->retryEvent != nullptr) {
                                 m_scheduler.cancelEvent(*(pi->retryEvent));
                                 pi->retryEvent = nullptr;
                              }
                              return true;
                              //
                          }
                          else {
                            if (pi->retryEvent == nullptr) {
                              NFD_LOG_DEBUG("LOST");

                              }
                              return false;
                          }
                      })
                   , el.second.end());
    //NFD_LOG_DEBUG("Current pipeline A " << el.second.size());
  }


  if (m_errorState == true) {
    resendPendingInterest();
    m_errorState = false;
  }
}

void
WeightedRandomStrategy::handleInterfaceStateChanged(shared_ptr<ndn::util::NetworkInterface>& ni,
                                                    ndn::util::NetworkInterfaceState oldState,
                                                    ndn::util::NetworkInterfaceState newState)
{
  if (newState == ndn::util::NetworkInterfaceState::RUNNING) {
    m_errorState = true;
    m_runningInterface = ni;
    resendPendingInterest();
  }
  else {
    auto list = m_interfaceInterests.find(ni->getName());
    if (list != m_interfaceInterests.end()) {
      m_errorState = true;
      m_runningInterface = ni;

      for (shared_ptr<PendingInterest>& pi : list->second) {
        if (pi->retryEvent != nullptr) {
          m_scheduler.cancelEvent(*(pi->retryEvent));
          pi->retryEvent = nullptr;
        }
      }

      m_rttMean = -1;
    }
  }
}

void WeightedRandomStrategy::resendPendingInterest()
{
  if (m_errorState) {
    if (m_runningInterface->getState() == ndn::util::NetworkInterfaceState::RUNNING) {
      auto list = m_interfaceInterests.find(m_runningInterface->getName());
      if (list != m_interfaceInterests.end()) {
        NFD_LOG_TRACE("Resend size " << list->second.size());
        for (shared_ptr<PendingInterest>& pi : list->second) {
          //NFD_LOG_DEBUG("Resend interest " << pi->pitEntry->getName());
          if (pi->retryEvent != nullptr) {
            m_scheduler.cancelEvent(*(pi->retryEvent));
          }
          pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())), bind(&WeightedRandomStrategy::retryInterest, this, pi->pitEntry, pi->outFace, time::steady_clock::now(), pi, true)));
          //this->sendInterest(el.pitEntry, el.outFace, true);
          //el.lastSent = time::steady_clock::now();
          //m_scheduler.scheduleEvent(time::milliseconds(200), bind(&WeightedRandomStrategy::resendPendingInterest, this));
        }
      }
    }
    //m_scheduler.scheduleEvent(time::milliseconds(200), bind(&WeightedRandomStrategy::resendPendingInterest, this));
  }
}

void
WeightedRandomStrategy::retryInterest(shared_ptr<pit::Entry> pitEntry, shared_ptr<Face> outFace,
                                      time::steady_clock::TimePoint sentTime, shared_ptr<PendingInterest> pi, bool now)
{
  if (pi->retryEvent != nullptr) {
    pi->retryEvent = nullptr;
    if (now) {
      NFD_LOG_TRACE("Resend single interest NOW" << pitEntry->getName());
      this->sendInterest(pitEntry, outFace, true);
      pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())), bind(&WeightedRandomStrategy::retryInterest, this, pitEntry, outFace, time::steady_clock::now(), pi, false)));
    }
    else {
      //time::milliseconds timeoutTime(time::duration_cast<time::milliseconds>(time::steady_clock::now() - sentTime).count());
      //if (timeoutTime.count() > getSendTimeout()) {
        NFD_LOG_TRACE("Resend single interest defer " << pitEntry->getName());
        this->sendInterest(pitEntry, outFace, false);
        pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())), bind(&WeightedRandomStrategy::retryInterest, this, pitEntry, outFace, time::steady_clock::now(), pi, false)));
      //}
      //else {
       // pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout()) - timeoutTime.count()), bind(&WeightedRandomStrategy::retryInterest, this, pitEntry, outFace, time::steady_clock::now(), pi, false)));
      //}
    }

  }
}



/*void
WeightedRandomStrategy::resendPendingInterestRetry()
{
  if( lastFace != nullptr && m_runningInterface !=nullptr && m_interfaceInterests.size() > 0) {
    if (m_runningInterface->getState() == ndn::util::NetworkInterfaceState::RUNNING) {
      auto list = m_interfaceInterests.find(m_runningInterface->getName());
      if (list != m_interfaceInterests.end()) {
        for (PendingInterest& el : list->second) {
          if (time::duration_cast<time::milliseconds>(time::steady_clock::now() - el.lastSent).count() > 500) {
            NFD_LOG_DEBUG("Resend interest timeout " << el.pitEntry->getName());
            this->sendInterest(el.pitEntry, el.outFace, true);
          }
        }
      }
    }
    m_scheduler.scheduleEvent(time::milliseconds(300), bind(&WeightedRandomStrategy::resendPendingInterestRetry, this));
  }
  else
    m_scheduler.scheduleEvent(time::milliseconds(300), bind(&WeightedRandomStrategy::resendPendingInterestRetry, this));

}*/

void
WeightedRandomStrategy::handleInterfaceAdded(const shared_ptr<ndn::util::NetworkInterface>& ni)
{
  ni->onStateChanged.connect(bind(&WeightedRandomStrategy::handleInterfaceStateChanged, this, ni, _1, _2));
}

void
WeightedRandomStrategy::handleInterfaceRemoved(const shared_ptr<ndn::util::NetworkInterface>& ni)
{

}

float
WeightedRandomStrategy::addRttMeasurement(float rtt)
{
  /*if (m_oldRtt.size() > m_nRttMean) {
    m_oldRtt.erase(m_oldRtt.begin());
  }
  m_oldRtt.push_back(rtt);

  if (m_oldRtt.size() > 2) {
    float newMean = 1;
    for(int i = 0; i < m_oldRtt.size(); i++) {
      newMean = (m_oldRtt[i] * rttMeanWeight.first) + (m_oldRtt[i+1] * rttMeanWeight.second);
    }
    m_lastRtt = rtt;
    m_rttMean = newMean;
  }
  else {
    m_lastRtt = rtt;
    m_rttMean = rtt;
  }*/

  if (m_rttMean == -1)
    m_rttMean = rtt;
  else
    m_rttMean = (m_rttMean * rttMeanWeight.first) + (rtt * rttMeanWeight.second);

  m_lastRtt = rtt;
  return m_rttMean;
}

float
WeightedRandomStrategy::getSendTimeout()
{
  if (m_rttMean == -1) {
    if (m_lastRtt == -1)
      m_rttMean = m_rtt0;
    else
      m_rttMean = m_lastRtt;
  }

  float rtt = m_rttMean * m_rttMulti;
  if (rtt > m_rttMax)
    return m_rttMax;

  return rtt;
}

int
WeightedRandomStrategy::getFaceWeight(const shared_ptr<Face>& face) const
{
  std::string nicName = face->getInterfaceName();
  auto it = m_interfacesInfo.find(nicName);
  if (it != m_interfacesInfo.end())
    return it->second.weight;
  else
    return 0;
}

} // namespace fw
} // namespace nfd
