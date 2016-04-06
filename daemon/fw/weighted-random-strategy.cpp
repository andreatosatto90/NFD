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
  , m_interfaceInterests()
  , m_errorState(false)
  , m_runningInterface()
{
  std::random_device rd;
  m_randomGen.seed(rd());

  getGlobalNetworkMonitor().onInterfaceAdded.connect(bind(&WeightedRandomStrategy::handleInterfaceAdded, this, _1));
  getGlobalNetworkMonitor().onInterfaceRemoved.connect(bind(&WeightedRandomStrategy::handleInterfaceRemoved, this, _1));

  rttMeanWeight.first = 0.3;  // Old value
  rttMeanWeight.second = 0.7; // New value
  m_rttMean = -1;
  m_lastRtt = -1;
  m_rttMulti = 1.2;
  m_rttMin = 20;
  m_rttMax = 1000;
  m_rtt0 = 150;

  m_nRttMean = 5;
  //lastRttTime = time::steady_clock::now();

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

      auto pi = make_shared<PendingInterest>(PendingInterest(outFace->getInterfaceName(), outFace, fibEntry, pitEntry, time::steady_clock::now(), nullptr));
      interestList->second.push_back(pi);
      pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())), bind(&WeightedRandomStrategy::retryInterest, this, pitEntry, outFace, time::steady_clock::now(), pi, false)));
      pi->deleteEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(interest.getInterestLifetime()), bind(&WeightedRandomStrategy::removePendingInterest, this, pi, pitEntry)));

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


  /*lp::NackHeader nackHeader;
  nackHeader.setReason(lp::NackReason::CONGESTION);
  this->sendNack(pitEntry, inFace, nackHeader);*/

  //auto interestList = m_interfaceInterests.insert({lastFace->getInterfaceName(), pendingInterests()}).first;
  //interestList->second.push_back(PendingInterest(lastFace->getInterfaceName(), lastFace, interest, fibEntry, pitEntry));
  //this->rejectPendingInterest(pitEntry);

  if ( lastFace != nullptr) {
    auto interestList = m_interfaceInterests.insert({lastFace->getInterfaceName(), pendingInterests()}).first;

    auto pi = make_shared<PendingInterest>(PendingInterest(lastFace->getInterfaceName(), lastFace, fibEntry, pitEntry, time::steady_clock::now(), nullptr));
    pi->retryEvent = nullptr;
    pi->deleteEvent = nullptr;
    interestList->second.push_back(pi);

    //pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())), bind(&WeightedRandomStrategy::retryInterest, this, pitEntry, lastFace, time::steady_clock::now(), pi, false)));
    //pi->deleteEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(interest.getInterestLifetime()), bind(&WeightedRandomStrategy::removePendingInterest, this, pi, pitEntry)));
    return;
  }

  NFD_LOG_TRACE("Interest rejected");

  lp::NackHeader nackHeader;
  nackHeader.setReason(lp::NackReason::DUPLICATE);
  this->sendNack(pitEntry, inFace, nackHeader);
  this->rejectPendingInterest(pitEntry);


  return;
}

void
WeightedRandomStrategy::beforeSatisfyInterest(shared_ptr<pit::Entry> pitEntry,
                                              const nfd::face::Face& inFace,
                                              const ndn::Data& data)
{
  if (pitEntry->hasValidLocalInRecord()) {

    NFD_LOG_DEBUG("Data received " << pitEntry->getName());
    bool hasOutRecords = true;
    time::milliseconds rtt = time::milliseconds(-1);
    pit::OutRecordCollection::const_iterator outRecord = pitEntry->getOutRecord(inFace);
    if (outRecord == pitEntry->getOutRecords().end()) { // no OutRecord
      /*NFD_LOG_TRACE(pitEntry->getInterest() << " dataFrom " << inFace.getId() <<
                    " no-out-record");*/
      hasOutRecords = false;
    }
    else {
      hasOutRecords = false;
      while (outRecord != pitEntry->getOutRecords().end()) {
        if (outRecord->getFace()->getId() != face::INVALID_FACEID ) {
          hasOutRecords = true;
        }
        ++outRecord;
      }
  //    rtt = time::duration_cast<time::milliseconds> (time::steady_clock::now() - outRecord->getLastRenewed());
  //    addRttMeasurement(rtt.count());
  //    NFD_LOG_DEBUG("Mean Rtt: " << m_rttMean);
      //NFD_LOG_DEBUG("Mean Rtt: " << m_rttMean);
    }

    if (!hasOutRecords)
       NFD_LOG_DEBUG("No valid out records");



    for (auto& el : m_interfaceInterests) {
      //NFD_LOG_DEBUG(" Remove pending received " << el.second.size());
      el.second.erase(std::remove_if(el.second.begin(), el.second.end(),
                        [&](shared_ptr<PendingInterest>& pi) {
                            if (pi->pitEntry->getInRecords().size() > 1)
                              NFD_LOG_DEBUG("DOUBLE");
                            if((pi->pitEntry == pitEntry) || !pi->pitEntry->hasValidLocalInRecord()) {
                                if(pi->pitEntry == pitEntry && hasOutRecords) {
                                    rtt = time::duration_cast<time::milliseconds> (time::steady_clock::now() - pi->lastSent);
                                    addRttMeasurement(rtt.count());
                                    //NFD_LOG_DEBUG("Mean Rtt: " << m_rttMean);
                                }

                                if (pi->retryEvent != nullptr) {
                                   m_scheduler.cancelEvent(*(pi->retryEvent));
                                   pi->retryEvent = nullptr;
                                }
                                if (pi->deleteEvent != nullptr) {
                                  m_scheduler.cancelEvent(*(pi->deleteEvent));
                                  pi->deleteEvent =nullptr;
                                }
                                return true;
                                //
                            }
                            else {
//                              if (pi->retryEvent == nullptr) {
//                                //NFD_LOG_DEBUG("LOST");

//                              }
                              return false;
                            }
                        })
                     , el.second.end());
      //NFD_LOG_DEBUG("Current pipeline A " << el.second.size());
    }

    if (hasOutRecords) // TODO we need the check?
      tracepoint(strategyLog, data_received, m_name.toUri().c_str(), pitEntry->getInterest().toUri().c_str(),
                 inFace.getId(), inFace.getInterfaceName().c_str(), rtt.count(), m_rttMean);

    //data.setTag(make_shared<lp::StrategyNotify>(5));
    if (m_errorState == true) {
      sendInvalidPendingInterest();
      m_errorState = false;
    }
  }
}

//void
//WeightedRandomStrategy::beforeExpirePendingInterest(shared_ptr<pit::Entry> pitEntry)
//{
//  NFD_LOG_DEBUG("Expired1: " << pitEntry->getName());

//  for (auto& el : m_interfaceInterests) {
//    NFD_LOG_DEBUG(" Actual size " << el.second.size());

//    //el.second.erase(std::remove(el.second.begin(), el.second.end(), pi), el.second.end());
//    el.second.erase(std::remove_if(el.second.begin(), el.second.end(),
//                      [&](shared_ptr<PendingInterest>& piTmp) {
//                          if(piTmp->pitEntry == pitEntry) {
//                              NFD_LOG_DEBUG("Done:  " << piTmp->pitEntry->getName() );
//                              if (piTmp->retryEvent != nullptr) {
//                                 m_scheduler.cancelEvent(*(piTmp->retryEvent));
//                                 piTmp->retryEvent = nullptr;

//                              }
//                              if (piTmp->deleteEvent != nullptr) {
//                                m_scheduler.cancelEvent(*(piTmp->deleteEvent));
//                                piTmp->deleteEvent =nullptr;
//                              }
//                              return true;
//                          }
//                          else {
//                              return false;
//                          }
//                      })
//                   , el.second.end());
//  }
//}

void
WeightedRandomStrategy::handleInterfaceStateChanged(shared_ptr<ndn::util::NetworkInterface>& ni,
                                                    ndn::util::NetworkInterfaceState oldState,
                                                    ndn::util::NetworkInterfaceState newState)
{
  if (newState == ndn::util::NetworkInterfaceState::RUNNING) {
    m_errorState = true;
    m_runningInterface = ni;
    sendInvalidPendingInterest();
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
          pi->invalid = true;
        }
      }

      m_rttMean = -1;
    }
  }
}

void WeightedRandomStrategy::sendInvalidPendingInterest()
{
  if (m_errorState) {
    if (m_runningInterface->getState() == ndn::util::NetworkInterfaceState::RUNNING) {
      auto list = m_interfaceInterests.find(m_runningInterface->getName());
      if (list != m_interfaceInterests.end()) {
        NFD_LOG_TRACE("Resend size " << list->second.size());
        for (shared_ptr<PendingInterest>& pi : list->second) {
          if (pi->invalid) {
            pi->lastSent = time::steady_clock::now();
            pi->invalid = false;
          }
          //NFD_LOG_DEBUG("Resend interest " << pi->pitEntry->getName());
//          if (pi->retryEvent != nullptr) {
//            m_scheduler.cancelEvent(*(pi->retryEvent));
//          }
          if (pi->retryEvent == nullptr) {
            pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())), bind(&WeightedRandomStrategy::retryInterest, this, pi->pitEntry, pi->outFace, time::steady_clock::now(), pi, true)));
          }
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
      if (pi->pitEntry->hasValidLocalInRecord()) {
        NFD_LOG_TRACE("Resend single interest NOW" << pitEntry->getName());
        this->sendInterest(pitEntry, outFace, true);
        pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())), bind(&WeightedRandomStrategy::retryInterest, this, pitEntry, outFace, time::steady_clock::now(), pi, false)));
      }
    }
    else {
      //time::milliseconds timeoutTime(time::duration_cast<time::milliseconds>(time::steady_clock::now() - sentTime).count());
      //if (timeoutTime.count() > getSendTimeout()) {
        //

        if (pi->pitEntry->hasValidLocalInRecord()) {
          for(auto& in : pitEntry->getInRecords()) {
            NFD_LOG_TRACE("Face in: " << in.getFace()->getId());
          }


          NFD_LOG_TRACE("Resend single interest defer " << pitEntry->getName() << " " << pitEntry->m_unsatisfyTimer);
          this->sendInterest(pitEntry, outFace, true);
          pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())), bind(&WeightedRandomStrategy::retryInterest, this, pitEntry, outFace, time::steady_clock::now(), pi, false)));
        }
      //}
      //else {
       // pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout()) - timeoutTime.count()), bind(&WeightedRandomStrategy::retryInterest, this, pitEntry, outFace, time::steady_clock::now(), pi, false)));
      //}
    }

  }
}

void
WeightedRandomStrategy::removePendingInterest(shared_ptr<PendingInterest>& pi, shared_ptr<pit::Entry> pitEntry)
{
  for (auto& el : m_interfaceInterests) {
    NFD_LOG_DEBUG(" Actual size " << el.second.size());

    //el.second.erase(std::remove(el.second.begin(), el.second.end(), pi), el.second.end());
    el.second.erase(std::remove_if(el.second.begin(), el.second.end(),
                      [&](shared_ptr<PendingInterest>& piTmp) {
                          if(piTmp == pi) {
                              NFD_LOG_DEBUG("Done:  " << pi->pitEntry->getName() );
                              if (pi->retryEvent != nullptr) {
                                 m_scheduler.cancelEvent(*(pi->retryEvent));
                                 pi->retryEvent = nullptr;

                              }
                              if (pi->deleteEvent != nullptr) {
                                m_scheduler.cancelEvent(*(pi->deleteEvent));
                                pi->deleteEvent =nullptr;
                              }
                              return true;
                          }
                          else {
                              return false;
                          }
                      })
                   , el.second.end());
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

  //lastRttTime = time::steady_clock::now();
  if (rtt < m_rttMin) {
    tracepoint(strategyLog, rtt_min, rtt);
    rtt = m_rttMin;
  }
  else if (rtt > m_rttMax) {
    tracepoint(strategyLog, rtt_max, rtt);
    rtt = m_rttMax;
  }
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
  // DEBUG only
  //if (time::duration_cast<time::milliseconds>(time::steady_clock::now() - lastRttTime) > time::seconds(10))
    //m_rttMean = -1;


  if (m_rttMean == -1) {
    if (m_lastRtt == -1)
      m_rttMean = m_rtt0;
    else
      m_rttMean = m_rtt0;
  }

  float rtt = m_rttMean * m_rttMulti;

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
