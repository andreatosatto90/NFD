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

#include "rtt-estimator-retries.hpp"
#include "strategies-tracepoint.hpp"

namespace nfd {

RttEstimatorRetries::RttEstimatorRetries()
{
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
}

float
RttEstimatorRetries::addRttMeasurement(std::vector<time::steady_clock::TimePoint> retries)
{
  float rtt = -1;
  if (retries.size() == 1) { // No retry
    rtt = (time::duration_cast<time::milliseconds> (time::steady_clock::now() - retries[0])).count();

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
  else if (retries.size() > 1) { // At least 1 retry
    for (int i = retries.size(); i > 0; i--) {
      rtt = (time::duration_cast<time::milliseconds> (time::steady_clock::now() - retries[i - 1])).count();
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

void
RttEstimatorRetries::reset()
{
  m_rttMean = -1;
  m_rttMinCalc = -1;
}

float
RttEstimatorRetries::computeRto()
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



} // namespace nfd

