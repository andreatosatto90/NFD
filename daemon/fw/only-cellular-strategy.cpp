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

#include "only-cellular-strategy.hpp"
#include "core/logger.hpp"

namespace nfd {
namespace fw {

NFD_LOG_INIT("OnlyCellularStrategy");

const Name OnlyCellularStrategy::STRATEGY_NAME("ndn:/localhost/nfd/strategy/only-cellular/%FD%01");
NFD_REGISTER_STRATEGY(OnlyCellularStrategy);

OnlyCellularStrategy::OnlyCellularStrategy(Forwarder& forwarder, const Name& name)
  : WeightedRandomStrategy(forwarder, name)
{
  // Set weight 1 to preferred interface, 0 to the other
  m_interfacesInfo.insert(std::make_pair("eth0",InterfaceInfo("eth0", 1)));
  m_interfacesInfo.insert(std::make_pair("wlan0",InterfaceInfo("wlan0", 0)));
}

OnlyCellularStrategy::~OnlyCellularStrategy()
{
}

} // namespace fw
} // namespace nfd
