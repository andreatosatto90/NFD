/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2014  Regents of the University of California,
 *                     Arizona Board of Regents,
 *                     Colorado State University,
 *                     University Pierre & Marie Curie, Sorbonne University,
 *                     Washington University in St. Louis,
 *                     Beijing Institute of Technology,
 *                     The University of Memphis
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
 **/

#ifndef NFD_DAEMON_FW_RTT_ESTIMATOR_RETRIES_HPP
#define NFD_DAEMON_FW_RTT_ESTIMATOR_RETRIES_HPP

#include "common.hpp"

namespace nfd {

class RttEstimatorRetries
{
public:


  RttEstimatorRetries();

  float
  addRttMeasurement(std::vector<time::steady_clock::TimePoint> retries);

  time::milliseconds
  computeRto();

  void
  reset();

  float
  getRttMean() const {
    return m_rttMean;
  }

  float
  getLastRtt() const {
    return m_lastRtt;
  }

private:
  // Rtt
  float m_rttMean;
  float m_rttVar;
  float m_rtt0;
  float m_rttMulti;
  float m_rttMax;
  float m_rttMin;
  float m_lastRtt;
  float m_rttMinCalc;

  uint32_t m_nSamples;
  std::vector<float> m_oldRtt;
  std::pair<float /*old*/, float /*new*/> rttMeanWeight;
  std::pair<float /*old*/, float /*new*/> rttVarWeight;
  time::steady_clock::TimePoint lastRttTime;

};

} // namespace nfd

#endif // NFD_DAEMON_FW_RTT_ESTIMATOR_RETRIES_HPP
