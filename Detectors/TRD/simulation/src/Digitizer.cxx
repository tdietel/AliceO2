// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#include "TRDSimulation/Digitizer.h"

using namespace o2::trd;

Digitizer::Digitizer()
{
}

Digitizer::~Digitizer()
{
}

void Digitizer::process(std::vector<o2::trd::HitType> const& hits, std::vector<o2::trd::Digit>& digits)
{
  // very basic implementation
  // for every hit seen, create a digit
  for (auto& hit : hits) {
    digits.emplace_back();
  }
}
