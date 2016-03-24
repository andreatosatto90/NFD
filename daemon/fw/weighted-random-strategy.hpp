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

public:

  class PendingInterest
  {
  public:
    PendingInterest(const std::string& interfaceName, const Face& inFace, const Interest& interest,
                    shared_ptr<fib::Entry> fibEntry, shared_ptr<pit::Entry> pitEntry)
      : interfaceName(interfaceName)
      , inFace(&inFace)
      , interest(&interest)
      , fibEntry(fibEntry)
      , pitEntry(pitEntry) {}


    std::string interfaceName;
    const Face* inFace; // TODO Deallocation problem
    const Interest* interest; // TODO Deallocation problem
    shared_ptr<fib::Entry> fibEntry;
    shared_ptr<pit::Entry> pitEntry;
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
  typedef std::vector<PendingInterest> pendingInterests;

  int
  getFaceWeight(const shared_ptr<nfd::face::Face>& face) const;

  void
  handleInterfaceStateChanged(const shared_ptr<ndn::util::NetworkInterface>& ni,
                              ndn::util::NetworkInterfaceState oldState,
                              ndn::util::NetworkInterfaceState newState);

protected:
  ndn::util::Scheduler m_scheduler;

  const Name& m_name;
  interfacesInfos m_interfacesInfo;
  std::unordered_map<std::string/*interfaceName*/,pendingInterests> m_interfaceInterests;
  std::mt19937 m_randomGen;

};

} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_WEIGHTED_RANDOM_STRATEGY_HPP
