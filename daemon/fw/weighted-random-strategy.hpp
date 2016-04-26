/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2014,  Regents of the University of California,
 *                      Arizona Board of Regents,
 *                      Colorado State University,
 *                      University Pierre & Marie Curie, Sorbonne University,
 *                      Washington University in St. Louis,
 *                      Beijing Institute of Technology,
 *                      The University of Memphis
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

#ifndef NFD_DAEMON_FW_WEIGHTED_RANDOM_STRATEGY_HPP
#define NFD_DAEMON_FW_WEIGHTED_RANDOM_STRATEGY_HPP

#include "strategy.hpp"

#include <ndn-cxx/util/scheduler.hpp>
#include <ndn-cxx/util/network-interface.hpp>

#include <daemon/face/transport.hpp>

namespace nfd {
namespace fw {

class WeightedRandomStrategy : public Strategy
{
public:
  WeightedRandomStrategy(Forwarder& forwarder, const Name& name);

  virtual
  ~WeightedRandomStrategy();

  virtual void
  afterReceiveInterest(const Face& inFace,
                       const Interest& interest,
                       shared_ptr<fib::Entry> fibEntry,
                       shared_ptr<pit::Entry> pitEntry) DECL_OVERRIDE;

  virtual void
  beforeSatisfyInterest(shared_ptr<pit::Entry> pitEntry,
                        const Face& inFace, const Data& data) DECL_OVERRIDE;

  //virtual void
  //beforeExpirePendingInterest(shared_ptr<pit::Entry> pitEntry) DECL_OVERRIDE;

public:

  class PendingInterest
  {
  public:
    PendingInterest(const std::string& interfaceName, shared_ptr<Face> outFace,
                    shared_ptr<fib::Entry> fibEntry, shared_ptr<pit::Entry> pitEntry,
                    shared_ptr<ndn::util::scheduler::EventId> retryEvent)
      : interfaceName(interfaceName)
      , outFace(outFace)
      , fibEntry(fibEntry)
      , pitEntry(pitEntry)
      , retryEvent(retryEvent)
      , invalid(false)
      {
      }


    std::string interfaceName;
    shared_ptr<Face> outFace;
    shared_ptr<fib::Entry> fibEntry;
    shared_ptr<pit::Entry> pitEntry;
    std::vector<time::steady_clock::TimePoint> retriesTimes;
    shared_ptr<ndn::util::scheduler::EventId> retryEvent;
    shared_ptr<ndn::util::scheduler::EventId> deleteEvent;
    bool invalid;
  };

protected:
  class InterfaceInfo
  {
  public:
    InterfaceInfo(std::string name, int weight) :
      name(name), weight(weight) {}

  public:
    std::string name;
    int weight; // TODO uint
  };

  typedef std::unordered_map<std::string, InterfaceInfo> interfacesInfos;
  typedef std::vector<shared_ptr<PendingInterest>> pendingInterests;

  int
  getFaceWeight(const shared_ptr<nfd::face::Face>& face) const;

  void
  handleInterfaceStateChanged(shared_ptr<ndn::util::NetworkInterface>& ni,
                              ndn::util::NetworkInterfaceState oldState,
                              ndn::util::NetworkInterfaceState newState);

  void
  handleFaceStateChanged(shared_ptr<ndn::util::NetworkInterface>& ni,
                         face::FaceState oldState,
                         face::FaceState newState);

  void
  sendInvalidPendingInterest();

  void
  resendPendingInterestRetry();

  void
  retryInterest(shared_ptr<pit::Entry> pitEntry, shared_ptr<Face> outFace,
                time::steady_clock::TimePoint sentTime, shared_ptr<PendingInterest> pi, bool now = false);

  void
  removePendingInterest(shared_ptr<PendingInterest>& pi, shared_ptr<pit::Entry> pitEntry);

  void
  deletePendingInterest(const shared_ptr<pit::Entry>& pitEntry);

  shared_ptr<PendingInterest>
  updatePendingInterest(const shared_ptr<pit::Entry>& pitEntry);

  bool
  insertPendingInterest(const Interest& interest, shared_ptr<Face> outFace,
                        shared_ptr<fib::Entry> fibEntry, shared_ptr<pit::Entry> pitEntry,
                        bool retryNow = true);

  void
  handleInterfaceAdded(const shared_ptr<ndn::util::NetworkInterface>& ni);

  void
  handleInterfaceRemoved(const shared_ptr<ndn::util::NetworkInterface>& ni);

  float
  addRttMeasurement(const shared_ptr<PendingInterest>& pi);

  float
  getSendTimeout();

protected:
  ndn::util::Scheduler m_scheduler;

  const Name& m_name;
  interfacesInfos m_interfacesInfo;

  std::unordered_map<std::string/*interfaceName*/,pendingInterests> m_interfaceInterests;
  std::mt19937 m_randomGen;

  bool m_errorState;
  shared_ptr<ndn::util::NetworkInterface> m_runningInterface;
  shared_ptr<Face> lastFace;
  shared_ptr<ndn::util::scheduler::EventId> retryEvent;

  // Rtt
  float m_rttMean;
  float m_rtt0;
  float m_rttMulti;
  float m_rttMax;
  float m_rttMin;
  float m_lastRtt;
  float m_rttMinCalc;

  uint32_t m_nSamples;
  std::vector<float> m_oldRtt;
  std::pair<float /*old*/, float /*new*/> rttMeanWeight;
  time::steady_clock::TimePoint lastRttTime;

  shared_ptr<signal::Connection> resendAllEvent;

};

} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_WEIGHTED_RANDOM_STRATEGY_HPP
