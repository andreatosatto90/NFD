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

#ifndef NFD_DAEMON_FW_RETRIES_STRATEGY_HPP
#define NFD_DAEMON_FW_RETRIES_STRATEGY_HPP

#include "strategy.hpp"

#include <ndn-cxx/util/scheduler.hpp>
#include <ndn-cxx/util/network-interface.hpp>

#include <daemon/face/transport.hpp>
#include "rtt-estimator-retries.hpp"


namespace nfd {
namespace fw {

class RetriesStrategy : public Strategy
{
public:

  class NextHopRetries
  {
  public:
    NextHopRetries(shared_ptr<Face> outFace)
      : outFace(outFace)
      , retryEvent(nullptr) {}

  public:

    shared_ptr<Face> outFace;
    shared_ptr<ndn::util::scheduler::EventId> retryEvent;
    std::vector<time::steady_clock::TimePoint> retriesTimes;
  };

  class PendingInterest
  {
  public:
    PendingInterest(shared_ptr<fib::Entry> fibEntry,
                    shared_ptr<pit::Entry> pitEntry)
      : pitEntry(pitEntry)
      {
      }

    shared_ptr<pit::Entry> pitEntry;
    shared_ptr<ndn::util::scheduler::EventId> deleteEvent;
    std::vector<NextHopRetries> nextHops;
  };

public:
  RetriesStrategy(Forwarder& forwarder, const Name& name);

  virtual
  ~RetriesStrategy();

  virtual void
  beforeSatisfyInterest(shared_ptr<pit::Entry> pitEntry,
                        const Face& inFace, const Data& data) DECL_OVERRIDE DECL_FINAL;

protected:

  void
  insertPendingInterest(const Interest& interest, shared_ptr<Face> outFace,
                        shared_ptr<fib::Entry> fibEntry, shared_ptr<pit::Entry> pitEntry);

  /**
   * Return true if all the retries have to done with the specified interface.
   * Use case: An interface state goes up and we need to know which interface has to do the retries
   */
  virtual bool
  isMainInterface(std::string interfaceName); // TODO better name

private:

  void
  sendPendingInterest(shared_ptr<pit::Entry> pitEntry,
                shared_ptr<Face> outFace,
                weak_ptr<PendingInterest> pi);

  void
  removePendingInterest(weak_ptr<PendingInterest> pi);

  /**
   * @brief updatePendingInterest
   * @param pitEntry
   * @return the updated pending interest, nullptr if no pending interest is found
   */
  shared_ptr<PendingInterest>
  updatePendingInterest(const shared_ptr<pit::Entry>& pitEntry, const ndn::Interest& interest);

  void
  handleInterfaceStateChanged(shared_ptr<ndn::util::NetworkInterface>& ni,
                              ndn::util::NetworkInterfaceState oldState,
                              ndn::util::NetworkInterfaceState newState);

  void
  resendAllPendingInterest(std::string interfaceName);

  void
  handleFaceStateChanged(shared_ptr<ndn::util::NetworkInterface>& ni,
                         face::FaceState oldState,
                         face::FaceState newState);

  void
  handleInterfaceAdded(const shared_ptr<ndn::util::NetworkInterface>& ni);

  void
  handleInterfaceRemoved(const shared_ptr<ndn::util::NetworkInterface>& ni);

protected:
  ndn::util::Scheduler m_scheduler;

private:
  const Name& m_name;
  std::vector<shared_ptr<PendingInterest>> m_pendingInterests; // TODO use PIT measurements (what about zombie?)

  std::map<std::string /*IntefaceName*/, RttEstimatorRetries> rttEstimators;

  time::milliseconds m_interestZombieTime; // TODO better name
};

} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_RETRIES_STRATEGY_HPP
