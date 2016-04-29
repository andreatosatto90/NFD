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

#include "retries-strategy.hpp"
#include "core/logger.hpp"
#include "strategies-tracepoint.hpp"
#include "core/global-io.hpp"
#include "core/global-network-monitor.hpp"


namespace nfd {
namespace fw {

NFD_LOG_INIT("RetriesStrategy");

RetriesStrategy::RetriesStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder, name)
  , m_scheduler(getGlobalIoService())
  , m_name(name)
  , m_pendingInterests()
  , m_runningInterface()
{
  getGlobalNetworkMonitor().onInterfaceAdded.connect(bind(&RetriesStrategy::handleInterfaceAdded, this, _1));
  getGlobalNetworkMonitor().onInterfaceRemoved.connect(bind(&RetriesStrategy::handleInterfaceRemoved, this, _1));

  lastFace = nullptr;
}

RetriesStrategy::~RetriesStrategy()
{
}

shared_ptr<RetriesStrategy::PendingInterest>
RetriesStrategy::updatePendingInterest(const shared_ptr<pit::Entry>& pitEntry)
{
  for (shared_ptr<PendingInterest>& pi : m_pendingInterests) {
    if (pi->pitEntry->getName() == pitEntry->getName()) {
      pi->pitEntry = pitEntry;
      pi->deleteEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(pitEntry->getInterest().getInterestLifetime().count() + 200),
                                                                                             bind(&RetriesStrategy::removePendingInterest, this, pi, pitEntry)));
      return pi;
    }
  }

  return nullptr;
}

bool
RetriesStrategy::insertPendingInterest(const Interest& interest,
                                              shared_ptr<nfd::face::Face> outFace,
                                              shared_ptr<fib::Entry> fibEntry,
                                              shared_ptr<pit::Entry> pitEntry,
                                              bool tryNow)
{
  auto pi = updatePendingInterest(pitEntry);
  bool isNew = false;

  //shared_ptr<PendingInterest> pi = nullptr;
  //deletePendingInterest(pitEntry);
  //bool isNew = true;

  if (pi == nullptr) {
    pi = make_shared<PendingInterest>(PendingInterest(fibEntry, pitEntry));
    pi->nextHops.push_back(outFace); //TODO all fib
    m_pendingInterests.push_back(pi);
    pi->deleteEvent = make_shared<ndn::util::scheduler::EventId>(
                        m_scheduler.scheduleEvent(time::milliseconds(interest.getInterestLifetime().count() + 100),
                                                  bind(&RetriesStrategy::removePendingInterest, this, pi, pitEntry)));
    isNew = true;
  }
  if (tryNow)
      retryInterest(pitEntry, outFace, pi); // TODO Parser should not consider the first retry tracepoint for each pending interest

  return isNew;
}

void
RetriesStrategy::beforeSatisfyInterest(shared_ptr<pit::Entry> pitEntry,
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

    // TODO break cycle after first element found
    m_pendingInterests.erase(std::remove_if(m_pendingInterests.begin(), m_pendingInterests.end(),
                      [&](shared_ptr<PendingInterest>& pi) {
                          if(pi->pitEntry == pitEntry) { // || !pi->pitEntry->hasValidLocalInRecord()
                              //piDelete = pi;
                              if(pi->pitEntry == pitEntry && hasOutRecords && !pi->nextHops[0].retriesTimes.empty()) {
                                  nRetries = pi->nextHops[0].retriesTimes.size() - 1;
                                  if (nRetries < 0)
                                    nRetries = 0;
                                  retrieveTime = (time::duration_cast<time::milliseconds> (time::steady_clock::now() - pi->nextHops[0].retriesTimes[0])).count();
                                  rtt = rttEstimators[pi->nextHops[0].outFace->getInterfaceName()].addRttMeasurement(pi->nextHops[0].retriesTimes);
                              }
                              for (auto nextHop : pi->nextHops) {
                                if (nextHop.retryEvent != nullptr) {
                                  m_scheduler.cancelEvent(*(nextHop.retryEvent));
                                  nextHop.retryEvent = nullptr;
                                }
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
                   , m_pendingInterests.end());


    if (hasOutRecords)
      tracepoint(strategyLog, data_received, m_name.toUri().c_str(), pitEntry->getInterest().toUri().c_str(),
                 inFace.getId(), inFace.getInterfaceName().c_str(), rtt, rttEstimators[inFace.getInterfaceName()].getRttMean(), nRetries, retrieveTime, rttEstimators[inFace.getInterfaceName()].getLastRtt());
    else {
      tracepoint(strategyLog, data_rejected, m_name.toUri().c_str(), pitEntry->getInterest().toUri().c_str(),
                 inFace.getId(), inFace.getInterfaceName().c_str(), rtt, rttEstimators[inFace.getInterfaceName()].getRttMean(), nRetries, retrieveTime, rttEstimators[inFace.getInterfaceName()].getLastRtt());
      NFD_LOG_DEBUG("Data rejected " << pitEntry->getName());
    }

    //NFD_LOG_WARN("Retries " << nRetries << " RTT " << rtt << " bounded " << m_lastRtt << " Mean " << m_rttMean << " Min " << m_rttMinCalc);

    //data.setTag(make_shared<lp::StrategyNotify>(5));
  }
}

void
RetriesStrategy::handleInterfaceStateChanged(shared_ptr<ndn::util::NetworkInterface>& ni,
                                                    ndn::util::NetworkInterfaceState oldState,
                                                    ndn::util::NetworkInterfaceState newState)
{
  if (newState == ndn::util::NetworkInterfaceState::RUNNING) {
    NFD_LOG_DEBUG("Interface UP");
    m_runningInterface = ni;

    if (m_pendingInterests.size() > 0)
      resendAllEvent = make_shared<signal::Connection>(lastFace->getTransport()->afterStateChange.connect(
                                bind(&RetriesStrategy::handleFaceStateChanged, this, ni, _1, _2)));
  }
  else {
    m_runningInterface = ni;

    for (shared_ptr<PendingInterest>& pi : m_pendingInterests) {
      auto it = std::find_if(pi->nextHops.begin(), pi->nextHops.end(),
                              [ni] (const NextHopRetries& nextHop) {
                                               return ni->getName() == nextHop.outFace->getInterfaceName();
                                             });
      if (it != pi->nextHops.end()) {
        m_scheduler.cancelEvent(*(it->retryEvent));
        it->retryEvent = nullptr;
        it->retriesTimes.clear();
      }
    }

    rttEstimators[ni->getName()].reset();
  }
}

void RetriesStrategy::handleFaceStateChanged(shared_ptr<ndn::util::NetworkInterface>& ni,
                                                    face::FaceState oldState, face::FaceState newState)
{
  if (resendAllEvent != nullptr && newState == face::TransportState::UP) {
    NFD_LOG_DEBUG("Transport UP");
    NFD_LOG_DEBUG("Resend all after interface down");
    if (lastFace != nullptr && lastFace->getState() == face::TransportState::UP) {
      NFD_LOG_TRACE("Resend size " << m_pendingInterests.size());
      for (shared_ptr<PendingInterest>& pi : m_pendingInterests) {
        //if (pi->retryEvent == nullptr) {
          //pi->retriesTimes.clear();
          retryInterest(pi->pitEntry, pi->nextHops[0].outFace, pi); // Change to right one
        //}
      }

    }
    resendAllEvent->disconnect();
    resendAllEvent = nullptr;
  }
}

void
RetriesStrategy::retryInterest(shared_ptr<pit::Entry> pitEntry, shared_ptr<Face> outFace,
                                     weak_ptr<PendingInterest> pi)
{
  shared_ptr<PendingInterest> newPi = pi.lock();
  if (newPi) {
    if (newPi->pitEntry->hasValidLocalInRecord()) {
      NFD_LOG_TRACE("Resend single interest NOW " << pitEntry->getName());
      this->sendInterest(pitEntry, outFace, true);
      auto it = std::find_if(newPi->nextHops.begin(), newPi->nextHops.end(),
                                   [outFace] (const NextHopRetries& nextHop) { return outFace == nextHop.outFace;});
      if (it != newPi->nextHops.end()) {
        if (it->retryEvent != nullptr)
           m_scheduler.cancelEvent(*(it->retryEvent));
        it->retryEvent = make_shared<ndn::util::scheduler::EventId>(m_scheduler.scheduleEvent(time::milliseconds(int(rttEstimators[outFace->getInterfaceName()].computeRto())),
                                                                    bind(&RetriesStrategy::retryInterest, this, pitEntry, outFace, newPi)));
        it->retriesTimes.push_back(time::steady_clock::now());
      }
      tracepoint(strategyLog, interest_sent_retry, m_name.toUri().c_str(), pitEntry->getName().toUri().c_str(),
                 outFace->getId(), outFace->getInterfaceName().c_str(), rttEstimators[outFace->getInterfaceName()].computeRto());

    }
  }
}

void
RetriesStrategy::removePendingInterest(weak_ptr<PendingInterest> pi, shared_ptr<pit::Entry> pitEntry)
{
  shared_ptr<PendingInterest> newPi = pi.lock();


  if (newPi) {
    for (auto nextHop : newPi->nextHops) {
      if (nextHop.retryEvent != nullptr) {
        m_scheduler.cancelEvent(*(nextHop.retryEvent));
        nextHop.retryEvent = nullptr;
      }
    }
    if (newPi->deleteEvent != nullptr) {
      m_scheduler.cancelEvent(*(newPi->deleteEvent));
      newPi->deleteEvent = nullptr;
    }

    NFD_LOG_DEBUG(" Actual size " << m_pendingInterests.size());
    //TODO exit after the first one is found
    m_pendingInterests.erase(std::remove_if(m_pendingInterests.begin(), m_pendingInterests.end(),
                      [&](shared_ptr<PendingInterest>& piTmp) {
                          if(piTmp->pitEntry == newPi->pitEntry) {
                              NFD_LOG_INFO("Done:  " << newPi->pitEntry->getName() );
                              return true;
                          }
                          else {
                              return false;
                          }
                      })
                   ,m_pendingInterests.end());

  }

}

void
RetriesStrategy::handleInterfaceAdded(const shared_ptr<ndn::util::NetworkInterface>& ni)
{
  ni->onStateChanged.connect(bind(&RetriesStrategy::handleInterfaceStateChanged, this, ni, _1, _2));
  rttEstimators.insert(std::make_pair(ni->getName(),RttEstimatorRetries()));
}

void
RetriesStrategy::handleInterfaceRemoved(const shared_ptr<ndn::util::NetworkInterface>& ni)
{
  // TODO disconnect state changed and remove rtt estimator
}

} // namespace fw
} // namespace nfd
