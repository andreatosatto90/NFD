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
  , m_interestZombieTime(time::milliseconds(100))
{
  getGlobalNetworkMonitor().onInterfaceAdded.connect(bind(&RetriesStrategy::handleInterfaceAdded, this, _1));
  getGlobalNetworkMonitor().onInterfaceRemoved.connect(bind(&RetriesStrategy::handleInterfaceRemoved, this, _1));
}

RetriesStrategy::~RetriesStrategy()
{
}

shared_ptr<RetriesStrategy::PendingInterest>
RetriesStrategy::updatePendingInterest(const shared_ptr<pit::Entry>& pitEntry, const Interest& interest)
{
  for (shared_ptr<PendingInterest>& pi : m_pendingInterests) {
    if (pi->pitEntry->getName() == pitEntry->getName()) {
      pi->pitEntry = pitEntry;
      if (pi->deleteEvent != nullptr)
        m_scheduler.cancelEvent(*(pi->deleteEvent));
      pi->deleteEvent = make_shared<ndn::util::scheduler::EventId>(
                          m_scheduler.scheduleEvent(time::milliseconds(interest.getInterestLifetime().count() + m_interestZombieTime.count()),
                                                    bind(&RetriesStrategy::removePendingInterest, this, pi)));
      return pi;
    }
  }

  return nullptr;
}

void
RetriesStrategy::insertPendingInterest(const Interest& interest,
                                       shared_ptr<nfd::face::Face> outFace,
                                       shared_ptr<fib::Entry> fibEntry,
                                       shared_ptr<pit::Entry> pitEntry)
{
  auto pi = updatePendingInterest(pitEntry, interest);

  if (pi == nullptr) { // New pending interest
    pi = make_shared<PendingInterest>(PendingInterest(fibEntry, pitEntry));

    const fib::NextHopList& nexthops = fibEntry->getNextHops();
    for (const fib::NextHop& nextHop : nexthops) {
      pi->nextHops.push_back(nextHop.getFace()); // TODO weak ptr to face?
    }

    m_pendingInterests.push_back(pi);
    pi->deleteEvent = make_shared<ndn::util::scheduler::EventId>(
                        m_scheduler.scheduleEvent(time::milliseconds(interest.getInterestLifetime().count() + m_interestZombieTime.count()),
                                                  bind(&RetriesStrategy::removePendingInterest, this, pi)));
  }
  // TODO we assume that the fib don't change during the execution, we should update next hops list

  if (outFace != nullptr)
    sendPendingInterest(pitEntry, outFace, pi);
}

void
RetriesStrategy::beforeSatisfyInterest(shared_ptr<pit::Entry> pitEntry,
                                       const nfd::face::Face& inFace,
                                       const ndn::Data& data)
{
  if (pitEntry->hasValidLocalInRecord()) {

    //NFD_LOG_INFO("Data received " << pitEntry->getName());
    bool hasOutRecords = false;
    pit::OutRecordCollection::const_iterator outRecord = pitEntry->getOutRecord(inFace);
    // Calculate rtt only if the pitEntry has at least one out record
    if (outRecord != pitEntry->getOutRecords().end()) {
      while (outRecord != pitEntry->getOutRecords().end()) {
        if (outRecord->getFace()->getId() != face::INVALID_FACEID) {
          hasOutRecords = true;
          break;
        }
        ++outRecord;
      }
    }
    //else NFD_LOG_TRACE(pitEntry->getInterest() << " dataFrom " << inFace.getId() << " no-out-record");

    // trace vars
    float rtt = -1;
    int nRetries = 0;
    int retrieveTime = -1;

    m_pendingInterests.erase(std::find_if(m_pendingInterests.begin(), m_pendingInterests.end(),
                                          [&](shared_ptr<PendingInterest>& pi) { // TODO unreadable, move out
                                              if(pi->pitEntry == pitEntry) { // || !pi->pitEntry->hasValidLocalInRecord()
                                                  for (auto& nextHop : pi->nextHops) {

                                                    if(nextHop.outFace->getId() != face::INVALID_FACEID && nextHop.outFace->getId() == inFace.getId()) {
                                                      if(pi->pitEntry == pitEntry && hasOutRecords && !nextHop.retriesTimes.empty()) {
                                                         nRetries = nextHop.retriesTimes.size() - 1;
                                                         if (nRetries < 0)
                                                           nRetries = 0;
                                                         retrieveTime = (time::duration_cast<time::milliseconds> (time::steady_clock::now() - nextHop.retriesTimes[0])).count();
                                                         rtt = rttEstimators[nextHop.outFace->getInterfaceName()].addRttMeasurement(nextHop.retriesTimes);
                                                      }
                                                      break;
                                                    }
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
                                          }));


    if (hasOutRecords)
      tracepoint(strategyLog, data_received, m_name.toUri().c_str(), pitEntry->getInterest().toUri().c_str(), inFace.getId(),
                 inFace.getInterfaceName().c_str(), rtt, rttEstimators[inFace.getInterfaceName()].getRttMean(),
                 nRetries, retrieveTime, rttEstimators[inFace.getInterfaceName()].getLastRtt());
    else {
      tracepoint(strategyLog, data_rejected, m_name.toUri().c_str(), pitEntry->getInterest().toUri().c_str(), inFace.getId(),
                 inFace.getInterfaceName().c_str(), rtt, rttEstimators[inFace.getInterfaceName()].getRttMean(),
                 nRetries, retrieveTime, rttEstimators[inFace.getInterfaceName()].getLastRtt());
      NFD_LOG_INFO("Data rejected " << pitEntry->getName());
    }
    //NFD_LOG_WARN("Retries " << nRetries << " RTT " << rtt << " bounded " << m_lastRtt << " Mean " << m_rttMean << " Min " << m_rttMinCalc);
  }
}

bool
RetriesStrategy::isMainInterface(std::string interfaceName)
{
  return true;
}

void
RetriesStrategy::handleInterfaceStateChanged(shared_ptr<ndn::util::NetworkInterface>& ni,
                                                    ndn::util::NetworkInterfaceState oldState,
                                                    ndn::util::NetworkInterfaceState newState)
{
  if (isMainInterface(ni->getName())) {
    rttEstimators[ni->getName()].reset(); // TODO We need it also here?

    if (newState == ndn::util::NetworkInterfaceState::RUNNING) {
      NFD_LOG_DEBUG("Interface UP, resend all to " << ni->getName());

      if (m_pendingInterests.size() > 0 ) {
        for (shared_ptr<PendingInterest>& pi : m_pendingInterests) {
          // TODO not working properly with more than 2 interfaces
          auto it = std::find_if(pi->nextHops.begin(), pi->nextHops.end(),
                                  [ni] (const NextHopRetries& nextHop) { return ni->getName() != nextHop.outFace->getInterfaceName(); });
          if (it != pi->nextHops.end()) {
            if (it->retryEvent != nullptr)
              m_scheduler.cancelEvent(*(it->retryEvent));
            it->retryEvent = nullptr;
            it->retriesTimes.clear();
          }
        }
        resendAllPendingInterest(ni->getName());
      }
    }
    else{ // The interface with highest priority is not running
      shared_ptr<Face> faceToSend = nullptr;
      for (shared_ptr<PendingInterest>& pi : m_pendingInterests) {

        auto it = std::find_if(pi->nextHops.begin(), pi->nextHops.end(),
                               [ni] (const NextHopRetries& nextHop) { return ni->getName() == nextHop.outFace->getInterfaceName();});

        if (it != pi->nextHops.end()) {
          if (it->retryEvent != nullptr)
            m_scheduler.cancelEvent(*(it->retryEvent));
          it->retryEvent = nullptr;
          it->retriesTimes.clear();
        }

        // TODO not working properly with more than 2 interfaces
        if (faceToSend == nullptr) {
          auto it2 = std::find_if(pi->nextHops.begin(), pi->nextHops.end(),
                                  [ni] (const NextHopRetries& nextHop) { return ni->getName() != nextHop.outFace->getInterfaceName();});

          if (it2 != pi->nextHops.end()) {
            faceToSend = it2->outFace;
          }
        }
      }
      rttEstimators[ni->getName()].reset();

      if (faceToSend != nullptr && faceToSend->getState() == face::TransportState::UP)
        resendAllPendingInterest(faceToSend->getInterfaceName());
    }
  }
}

void
RetriesStrategy::resendAllPendingInterest(std::string interfaceName)
{
  NFD_LOG_DEBUG("Resend size " << m_pendingInterests.size() << " to " << interfaceName);
  for (shared_ptr<PendingInterest>& pi : m_pendingInterests) {
    for (auto& nextHop : pi->nextHops) {
      auto& outFace = nextHop.outFace;
      if (outFace->getId() != face::INVALID_FACEID && outFace->getInterfaceName() == interfaceName)
        sendPendingInterest(pi->pitEntry, nextHop.outFace, pi);
      else {
        if (nextHop.retryEvent != nullptr) {
          m_scheduler.cancelEvent(*(nextHop.retryEvent));
          nextHop.retryEvent = nullptr;
        }
      }
    }
  }
}

void RetriesStrategy::handleFaceStateChanged(shared_ptr<ndn::util::NetworkInterface>& ni,
                                                    face::FaceState oldState, face::FaceState newState)
{
  /*if (resendAllEvent != nullptr && newState == face::TransportState::UP) {
    NFD_LOG_DEBUG("Transport UP");
    NFD_LOG_DEBUG("Resend all after interface down");
    if (lastFace != nullptr && lastFace->getState() == face::TransportState::UP) {
      NFD_LOG_TRACE("Resend size " << m_pendingInterests.size());
      for (shared_ptr<PendingInterest>& pi : m_pendingInterests) {
        //if (pi->retryEvent == nullptr) {
          //pi->retriesTimes.clear();
          for (auto& nextHop : pi->nextHops) {
            auto& outFace = nextHop.outFace;
            if (outFace->getId() != face::INVALID_FACEID && outFace->getInterfaceName() == ni->getName()) // TODO stop other retries and choose interface based on priority
              retryInterest(pi->pitEntry, nextHop.outFace, pi); // Change to right one
          }
        //}
      }

    }
    resendAllEvent->disconnect();
    resendAllEvent = nullptr;
  }*/
}

void
RetriesStrategy::sendPendingInterest(shared_ptr<pit::Entry> pitEntry, shared_ptr<Face> outFace, weak_ptr<PendingInterest> pi)
{
  shared_ptr<PendingInterest> newPi = pi.lock();
  if (newPi) {
    if (newPi->pitEntry->hasValidLocalInRecord()) {

      auto it = std::find_if(newPi->nextHops.begin(), newPi->nextHops.end(),
                             [outFace] (const NextHopRetries& nextHop) { return outFace == nextHop.outFace;});

      if (it != newPi->nextHops.end()) {
        this->sendInterest(pitEntry, outFace, true);
        it->retriesTimes.push_back(time::steady_clock::now());

        if (it->retryEvent != nullptr)
           m_scheduler.cancelEvent(*(it->retryEvent));
        it->retryEvent = make_shared<ndn::util::scheduler::EventId>(
                           m_scheduler.scheduleEvent(rttEstimators[outFace->getInterfaceName()].computeRto(),
                           bind(&RetriesStrategy::sendPendingInterest, this, pitEntry, outFace, newPi)));

        tracepoint(strategyLog, interest_sent, pitEntry->getName().toUri().c_str(),
                   outFace->getId(), outFace->getInterfaceName().c_str(), rttEstimators[outFace->getInterfaceName()].computeRto().count());
        NFD_LOG_DEBUG("Interest to interface "<< outFace->getInterfaceName());
      }
      else
        NFD_LOG_WARN("Pending interest has no face to the selected interface");
    }
    else
      removePendingInterest(pi);
  }
}

void
RetriesStrategy::removePendingInterest(weak_ptr<PendingInterest> pi)
{
  shared_ptr<PendingInterest> newPi = pi.lock();

  if (newPi && !pi.expired()) {
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

    if (m_pendingInterests.size() > 0) {
      NFD_LOG_TRACE("Removed interest, actual size " << m_pendingInterests.size());

      m_pendingInterests.erase(std::find_if(m_pendingInterests.begin(), m_pendingInterests.end(),
                                            [&](shared_ptr<PendingInterest>& piTmp) { return piTmp->pitEntry == newPi->pitEntry ? true : false;}));
    }
  }
}

void
RetriesStrategy::handleInterfaceAdded(const shared_ptr<ndn::util::NetworkInterface>& ni)
{
  ni->onStateChanged.connect(bind(&RetriesStrategy::handleInterfaceStateChanged, this, ni, _1, _2));
  rttEstimators.insert(std::make_pair(ni->getName(), RttEstimatorRetries()));
}

void
RetriesStrategy::handleInterfaceRemoved(const shared_ptr<ndn::util::NetworkInterface>& ni)
{
  // TODO disconnect state changed and remove rtt estimator
}

} // namespace fw
} // namespace nfd
