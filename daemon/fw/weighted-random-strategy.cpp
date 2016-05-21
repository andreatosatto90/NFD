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
  m_rttMulti = 2;
  m_rttMin = 10;
  m_rttMax = 1000;
  m_rtt0 = 250;
  m_rttMinCalc = -1;

  m_nSamples = 5;
  //lastRttTime = time::steady_clock::now();

  lastFace = nullptr;

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
WeightedRandomStrategy::deletePendingInterest(const shared_ptr<pit::Entry>& pitEntry)
{
  for (auto& el : m_interfaceInterests) {
    el.second.erase(std::remove_if(el.second.begin(), el.second.end(),
                      [&](shared_ptr<PendingInterest>& pi) {
                          if(pi->pitEntry == pitEntry) {
                              NFD_LOG_DEBUG("Delete retrasmission");
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
                          else
                            return false;
                      })
                   , el.second.end());
  }
}

shared_ptr<WeightedRandomStrategy::PendingInterest>
WeightedRandomStrategy::updatePendingInterest(const shared_ptr<pit::Entry>& pitEntry)
{
  for (auto& el : m_interfaceInterests) {
    for (shared_ptr<PendingInterest>& pi : el.second) {
      if (pi->pitEntry->getName() == pitEntry->getName()) {
        pi->pitEntry = pitEntry;
        pi->deleteEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(pitEntry->getInterest().getInterestLifetime().count() + 200),
                                                                                               bind(&WeightedRandomStrategy::removePendingInterest, this, pi, pitEntry)));
        return pi;
      }
    }
  }
  return nullptr;
}

bool
WeightedRandomStrategy::insertPendingInterest(const Interest& interest,
                                              shared_ptr<nfd::face::Face> outFace,
                                              shared_ptr<fib::Entry> fibEntry,
                                              shared_ptr<pit::Entry> pitEntry,
                                              bool retryNow)
{
  auto pi = updatePendingInterest(pitEntry);
  bool isNew = false;

  //shared_ptr<PendingInterest> pi = nullptr;
  //deletePendingInterest(pitEntry);
  //bool isNew = true;

  if (pi == nullptr) {
    auto interestList = m_interfaceInterests.insert({outFace->getInterfaceName(), pendingInterests()}).first;
    pi = make_shared<PendingInterest>(PendingInterest(outFace->getInterfaceName(), outFace, fibEntry, pitEntry, nullptr));
    interestList->second.push_back(pi);
    pi->deleteEvent = make_shared<ndn::util::scheduler::EventId>(
                        m_scheduler.scheduleEvent(time::milliseconds(interest.getInterestLifetime().count() + 100),
                                                  bind(&WeightedRandomStrategy::removePendingInterest, this, pi, pitEntry)));
    isNew = true;
  }
  if (retryNow) {
    pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(
                       m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())),
                                                 bind(&WeightedRandomStrategy::retryInterest, this, pitEntry, outFace, time::steady_clock::now(), pi, false)));
    pi->retriesTimes.push_back(time::steady_clock::now());
  }

  return isNew;
}

void
WeightedRandomStrategy::afterReceiveInterest(const Face& inFace,
                                             const Interest& interest,
                                             shared_ptr<fib::Entry> fibEntry,
                                             shared_ptr<pit::Entry> pitEntry)
{

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


      insertPendingInterest(interest, outFace, fibEntry, pitEntry, true);

      tracepoint(strategyLog, interest_sent, interest.toUri().c_str(),
                 outFace->getId(), outFace->getInterfaceName().c_str(), getSendTimeout());
      lastFace = outFace;
      return;
    }
    else
      NFD_LOG_TRACE("No eligible faces 1");
  }
  else
    NFD_LOG_TRACE("No eligible faces 2");


  if (lastFace != nullptr) {

    insertPendingInterest(interest, lastFace, fibEntry, pitEntry, false);

    tracepoint(strategyLog, interest_sent, interest.toUri().c_str(),
               lastFace->getId(), lastFace->getInterfaceName().c_str(), -2);

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

    NFD_LOG_TRACE("Data received " << pitEntry->getName());
    bool hasOutRecords = true;
    float rtt = -1;
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
    }

    if (!hasOutRecords)
       NFD_LOG_DEBUG("No valid out records");

    int nRetries = 0;
    int retrieveTime = -1;

    for (auto& el : m_interfaceInterests) {
      el.second.erase(std::remove_if(el.second.begin(), el.second.end(),
                        [&](shared_ptr<PendingInterest>& pi) {
                            if(pi->pitEntry == pitEntry) { // || !pi->pitEntry->hasValidLocalInRecord()

                                if(pi->pitEntry == pitEntry && hasOutRecords && !pi->retriesTimes.empty()) {                  
                                    nRetries = pi->retriesTimes.size() - 1;
                                    if (nRetries < 0)
                                      nRetries = 0;
                                    retrieveTime = (time::duration_cast<time::milliseconds> (time::steady_clock::now() - pi->retriesTimes[0])).count();
                                    rtt = addRttMeasurement(pi);
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
                            }
                            else {
                              return false;
                            }
                        })
                     , el.second.end());
    }

    if (hasOutRecords)
      tracepoint(strategyLog, data_received, m_name.toUri().c_str(), pitEntry->getInterest().toUri().c_str(),
                 inFace.getId(), inFace.getInterfaceName().c_str(), rtt, m_rttMean, nRetries, retrieveTime, m_lastRtt);
    else {
      tracepoint(strategyLog, data_rejected, m_name.toUri().c_str(), pitEntry->getInterest().toUri().c_str(),
                 inFace.getId(), inFace.getInterfaceName().c_str(), rtt, m_rttMean, nRetries, retrieveTime, m_lastRtt);
      NFD_LOG_DEBUG("Data rejected " << pitEntry->getName());
    }

    //NFD_LOG_WARN("Retries " << nRetries << " RTT " << rtt << " bounded " << m_lastRtt << " Mean " << m_rttMean << " Min " << m_rttMinCalc);

    //data.setTag(make_shared<lp::StrategyNotify>(5));
    if (m_errorState == true) {
      //sendInvalidPendingInterest();
      m_errorState = false;
    }
  }
}

void
WeightedRandomStrategy::handleInterfaceStateChanged(shared_ptr<ndn::util::NetworkInterface>& ni,
                                                    ndn::util::NetworkInterfaceState oldState,
                                                    ndn::util::NetworkInterfaceState newState)
{
  if (newState == ndn::util::NetworkInterfaceState::RUNNING) {
    NFD_LOG_DEBUG("Interface UP");
    m_errorState = true;
    m_runningInterface = ni;

    if (lastFace != nullptr)
      resendAllEvent = make_shared<signal::Connection>(lastFace->getTransport()->afterStateChange.connect(
                                bind(&WeightedRandomStrategy::handleFaceStateChanged, this, ni, _1, _2)));


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
      m_rttMinCalc = -1;
    }
  }
}

void WeightedRandomStrategy::handleFaceStateChanged(shared_ptr<ndn::util::NetworkInterface>& ni,
                                                    face::FaceState oldState, face::FaceState newState)
{
  if (resendAllEvent != nullptr && newState == face::TransportState::UP) {
    NFD_LOG_DEBUG("Transport UP");
    sendInvalidPendingInterest();
    resendAllEvent->disconnect();
    resendAllEvent = nullptr;
  }
}

void
WeightedRandomStrategy::sendInvalidPendingInterest()
{
  NFD_LOG_DEBUG("Resend all after interface down");
  if (m_errorState) {
    if (lastFace != nullptr && lastFace->getState() == face::TransportState::UP) {
      auto list = m_interfaceInterests.find(m_runningInterface->getName());
      if (list != m_interfaceInterests.end()) {
        NFD_LOG_TRACE("Resend size " << list->second.size());
        for (shared_ptr<PendingInterest>& pi : list->second) {
          if (pi->invalid) {
            pi->retriesTimes.clear();
            pi->invalid = false;
          }
          //NFD_LOG_DEBUG("Resend interest " << pi->pitEntry->getName());
//          if (pi->retryEvent != nullptr) {
//            m_scheduler.cancelEvent(*(pi->retryEvent));
//          }
          if (pi->retryEvent == nullptr) {
            retryInterest(pi->pitEntry, pi->outFace, time::steady_clock::now(), pi, true);
            //pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())), bind(&WeightedRandomStrategy::retryInterest, this, pi->pitEntry, pi->outFace, time::steady_clock::now(), pi, true)));
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
  if (now) {
      if (pi->pitEntry->hasValidLocalInRecord()) {
        NFD_LOG_TRACE("Resend single interest NOW" << pitEntry->getName());
        this->sendInterest(pitEntry, outFace, true);
        pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())), bind(&WeightedRandomStrategy::retryInterest, this, pitEntry, outFace, time::steady_clock::now(), pi, false)));
        pi->retriesTimes.push_back(time::steady_clock::now());
        tracepoint(strategyLog, interest_sent, pitEntry->getName().toUri().c_str(),
                   outFace->getId(), outFace->getInterfaceName().c_str(), getSendTimeout());

      }
  }
  else if (pi->retryEvent != nullptr) {
    pi->retryEvent = nullptr;

    if (pi->pitEntry->hasValidLocalInRecord()) {
//      for(auto& in : pitEntry->getInRecords()) {
//        NFD_LOG_TRACE("Face in: " << in.getFace()->getId());
//      }


      NFD_LOG_TRACE("Resend single interest defer " << pitEntry->getName() << " " << pitEntry->m_unsatisfyTimer);
      this->sendInterest(pitEntry, outFace, true);
      pi->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(getSendTimeout())), bind(&WeightedRandomStrategy::retryInterest, this, pitEntry, outFace, time::steady_clock::now(), pi, false)));
      pi->retriesTimes.push_back(time::steady_clock::now());
      tracepoint(strategyLog, interest_sent, pitEntry->getName().toUri().c_str(),
                 outFace->getId(), outFace->getInterfaceName().c_str(), getSendTimeout());
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
                                pi->deleteEvent = nullptr;
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
WeightedRandomStrategy::addRttMeasurement(const shared_ptr<PendingInterest>& pi)
{
  float rtt = -1;
  if (pi->retriesTimes.size() == 1) { // No retry
    rtt = (time::duration_cast<time::milliseconds> (time::steady_clock::now() - pi->retriesTimes[0])).count();

    if (m_rttMinCalc == -1) {
      m_rttMinCalc = rtt;
      tracepoint(strategyLog, rtt_min_calc, m_rttMinCalc);
    }
    else if (rtt < m_rttMinCalc) {
      m_rttMinCalc = rtt;
      tracepoint(strategyLog, rtt_min_calc, m_rttMinCalc);
    }
    /*else {
      m_rttMinCalc = (m_rttMinCalc * rttMeanWeight.first) + (rtt * rttMeanWeight.second);
      tracepoint(strategyLog, rtt_min_calc, m_rttMinCalc);
    }*/


    //NFD_LOG_DEBUG("Rtt min calc: " << m_rttMinCalc);
  }
  else if (pi->retriesTimes.size() > 1) { // At least 1 retry
    for (int i = pi->retriesTimes.size(); i > 0; i--) {
      rtt = (time::duration_cast<time::milliseconds> (time::steady_clock::now() - pi->retriesTimes[i - 1])).count();
      if (m_rttMinCalc != -1 && rtt >= m_rttMinCalc)
        break;
    }
  }
  else
    return -1; // This should not happen

  float rttOriginal = rtt;

  if (m_rttMinCalc == -1 && rtt < m_rttMin) {
    tracepoint(strategyLog, rtt_min, rtt);
    rtt = m_rttMin;
  }
  else if (m_rttMinCalc != -1 && rtt < m_rttMinCalc) {
    tracepoint(strategyLog, rtt_min, rtt);
    rtt = m_rttMinCalc;
  }

  if (rtt > m_rttMax) {
    tracepoint(strategyLog, rtt_max, rtt);
    rtt = m_rttMax;
  }



  while (m_oldRtt.size() > m_nSamples) {
    m_oldRtt.erase(m_oldRtt.begin());
  }
  m_oldRtt.push_back(rtt);

  float newMean = m_oldRtt[0];
  for(uint32_t i = 1; i < m_oldRtt.size(); i++) {
    newMean = (newMean * rttMeanWeight.first) + (m_oldRtt[i] * rttMeanWeight.second);
  }

  m_lastRtt = rtt;
  m_rttMean = newMean;


  /*if (m_rttMean == -1)
    m_rttMean = rtt;
  else
    m_rttMean = (m_rttMean * rttMeanWeight.first) + (rtt * rttMeanWeight.second);
  m_lastRtt = rtt;*/


  return rttOriginal;
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
      m_rttMean = m_rtt0; // last rtt
  }

  float rtt = m_rttMean * m_rttMulti;

  if (rtt < 0) // TODO
    return 1;

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
