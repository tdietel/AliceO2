// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#include "TPCDriftTimeDigitizerSpec.h"
#include <FairMQLogger.h>
#include <TMessage.h> // object serialization
#include <cassert>
#include <cstring> // memcpy
#include <memory>  // std::unique_ptr
#include <string>  // std::string
#include "Framework/ControlService.h"
#include "Framework/DataProcessorSpec.h"
#include "Framework/DataRefUtils.h"
#include "Framework/Lifetime.h"
#include "Headers/DataHeader.h"
#include "Steer/HitProcessingManager.h"
#include <TPCSimulation/Digitizer.h>
#include <TPCSimulation/DigitizerTask.h>
#include <functional>
#include "TPCSimulation/Point.h"
#include "TPCSimulation/ElectronTransport.h"
#include "TPCSimulation/HitDriftFilter.h"
#include "TPCSimulation/SpaceCharge.h"
#include "TStopwatch.h"
#include <sstream>
#include <algorithm>
#include <utility>
#include "TPCBase/CDBInterface.h"
#include "DataFormatsTPC/TPCSectorHeader.h"
#include "DataFormatsParameters/GRPObject.h"
#include "CommonDataFormat/RangeReference.h"

using namespace o2::framework;
using SubSpecificationType = o2::framework::DataAllocator::SubSpecificationType;
using DigiGroupRef = o2::dataformats::RangeReference<int, int>;

namespace o2
{
namespace steer
{

std::string getBranchNameLeft(int sector)
{
  std::stringstream branchnamestreamleft;
  branchnamestreamleft << "TPCHitsShiftedSector" << int(o2::TPC::Sector::getLeft(o2::TPC::Sector(sector)));
  return branchnamestreamleft.str();
}

std::string getBranchNameRight(int sector)
{
  std::stringstream branchnamestreamright;
  branchnamestreamright << "TPCHitsShiftedSector" << sector;
  return branchnamestreamright.str();
}

DataProcessorSpec getTPCDriftTimeDigitizer(int channel, bool cachehits)
{
  TChain* simChain = new TChain("o2sim");

  /// For the time being use the defaults for the CDB
  auto& cdb = o2::TPC::CDBInterface::instance();
  cdb.setUseDefaults();
  static int firstChannel = -1;
  //
  auto simChains = std::make_shared<std::vector<TChain*>>();
  auto digitizertask = std::make_shared<o2::TPC::DigitizerTask>();

  auto digitArray = std::make_shared<std::vector<o2::TPC::Digit>>();

  auto mcTruthArray = std::make_shared<o2::dataformats::MCTruthContainer<o2::MCCompLabel>>();

  auto doit = [simChain, simChains, digitizertask, digitArray, mcTruthArray, channel](ProcessingContext& pc) {
    static int callcounter = 0;

    callcounter++;
    static bool finished = false;
    if (finished) {
      return;
    }

    if (firstChannel == channel) {
      auto roMode = digitizertask->isContinuousReadout() ? o2::parameters::GRPObject::CONTINUOUS : o2::parameters::GRPObject::PRESENT;
      LOG(INFO) << "TPC: Sending ROMode= " << (digitizertask->isContinuousReadout() ? "Continuous" : "Triggered")
                << " to GRPUpdater from channel " << firstChannel;
      pc.outputs().snapshot(Output{ "TPC", "ROMode", 0, Lifetime::Timeframe }, roMode);
      firstChannel = -10; // forbid further sending
    }

    // extract which sector to treat
    using TPCSectorHeader = o2::TPC::TPCSectorHeader;
    auto const* sectorHeader = DataRefUtils::getHeader<TPCSectorHeader*>(pc.inputs().get("collisioncontext"));
    if (sectorHeader == nullptr) {
      LOG(ERROR) << "TPC sector header missing, skipping processing";
      return;
    }
    auto sector = sectorHeader->sector;
    LOG(INFO) << "GOT ASSIGNED SECTOR " << sector;
    // the active sectors need to be propagated
    uint64_t activeSectors = 0;
    activeSectors = sectorHeader->activeSectors;

    // ===| open file and register branches |=====================================
    // this is done at the moment for each worker function invocation
    // TODO: make this nicer or let the write service be handled outside
    auto digitArrayRaw = digitArray.get();
    auto mcTruthArrayRaw = mcTruthArray.get();

    // lambda that snapshots digits to be sent out; prepares and attaches header with sector information
    auto snapshotDigits = [sector, &pc, channel, activeSectors](std::vector<o2::TPC::Digit> const& digits) {
      o2::TPC::TPCSectorHeader header{ sector };
      header.activeSectors = activeSectors;
      // note that snapshoting only works with non-const references (to be fixed?)
      pc.outputs().snapshot(Output{ "TPC", "DIGITS", static_cast<SubSpecificationType>(channel), Lifetime::Timeframe,
                                    header },
                            const_cast<std::vector<o2::TPC::Digit>&>(digits));
    };
    // lambda that snapshots labels to be sent out; prepares and attaches header with sector information
    auto snapshotLabels = [&sector, &pc, &channel, activeSectors](o2::dataformats::MCTruthContainer<o2::MCCompLabel> const& labels) {
      o2::TPC::TPCSectorHeader header{ sector };
      header.activeSectors = activeSectors;
      pc.outputs().snapshot(Output{ "TPC", "DIGITSMCTR", static_cast<SubSpecificationType>(channel),
                                    Lifetime::Timeframe, header },
                            const_cast<o2::dataformats::MCTruthContainer<o2::MCCompLabel>&>(labels));
    };
    // lambda that snapshots digits grouping (triggers) to be sent out; prepares and attaches header with sector information
    auto snapshotEvents = [sector, &pc, channel, activeSectors](const std::vector<DigiGroupRef>& events) {
      o2::TPC::TPCSectorHeader header{ sector };
      header.activeSectors = activeSectors;
      LOG(INFO) << "Send TRIGGERS for sector " << sector << " channel " << channel << " | size " << events.size();
      pc.outputs().snapshot(Output{ "TPC", "DIGTRIGGERS", static_cast<SubSpecificationType>(channel), Lifetime::Timeframe,
                                    header },
                            const_cast<std::vector<DigiGroupRef>&>(events));
    };

    // no more tasks can be marked with a negative sector
    if (sector < 0) {
      // we are ready to quit or we received a NOP

      // notify channels further down the road that we are done (why do I have to send digitArray here????)
      // (we are essentially sending empty messages with a header containing the stop signal)
      digitArrayRaw->clear();
      mcTruthArrayRaw->clear();
      std::vector<DigiGroupRef> evAccDummy;

      snapshotEvents(evAccDummy);
      snapshotDigits(*digitArrayRaw);
      snapshotLabels(*mcTruthArrayRaw);
      if (sector == -1) {
        pc.services().get<ControlService>().readyToQuit(false);
        finished = true;
      }
      return;
    }

    // obtain collision contexts
    auto context = pc.inputs().get<o2::steer::RunContext*>("collisioncontext");
    auto& timesview = context->getEventRecords();
    LOG(DEBUG) << "GOT " << timesview.size() << " COLLISSION TIMES";

    // if there is nothing ... return
    if (timesview.size() == 0) {
      return;
    }

    TStopwatch timer;
    timer.Start();

    // detect number of possible drift times (remember that a drift
    // time is convenient unit for digit pileup), any multiple of this
    // unit should also be ok
    const auto TPCDRIFT = 100000;
    double maxtime = 0;
    for (auto e : timesview) {
      maxtime = std::max(maxtime, e.timeNS);
    }

    std::vector<std::vector<o2::TPC::HitGroup>*> hitvectorsleft;  // "TPCHitVector"
    std::vector<o2::TPC::TPCHitGroupID> hitidsleft;               // "TPCHitIDs"
    std::vector<std::vector<o2::TPC::HitGroup>*> hitvectorsright; // "TPCHitVector"
    std::vector<o2::TPC::TPCHitGroupID> hitidsright;              // "TPCHitIDs"

    // try to access event parts information
    const auto& parts = context->getEventParts();
    // query max size of entries
    const int maxsourcespercollision = context->getMaxNumberParts();
    const int numberofcollisions = context->getNCollisions();
    const int maxnumberofentries = maxsourcespercollision * numberofcollisions;
    LOG(INFO) << " MAX ENTRIES " << maxnumberofentries;
    hitvectorsleft.resize(maxnumberofentries, nullptr);
    hitvectorsright.resize(maxnumberofentries, nullptr);

    // this is to accum digits from different drift times
    std::vector<o2::TPC::Digit> digitAccum;
    o2::dataformats::MCTruthContainer<o2::MCCompLabel> labelAccum;
    std::vector<DigiGroupRef> eventAccum;

    if (digitizertask->isContinuousReadout()) {
      // minimum 2 drifts is a safe bet; an electron might
      // need 1 full drift and might hence land in the second drift time
      auto ndrifts = 2 + (int)(maxtime / TPCDRIFT);
      for (int drift = 1; drift <= ndrifts; ++drift) {
        auto starttime = (drift - 1) * TPCDRIFT;
        auto endtime = drift * TPCDRIFT;
        LOG(DEBUG) << "STARTTIME " << starttime << " ENDTIME " << endtime;
        digitizertask->setStartTime(starttime);
        digitizertask->setEndTime(endtime);

        hitidsleft.clear();
        hitidsright.clear();

        // obtain candidate hit(ids) for this time range --> left
        o2::TPC::getHits(*simChains.get(), *context.get(), hitvectorsleft, hitidsleft, getBranchNameLeft(sector).c_str(),
                         starttime, endtime, o2::TPC::calcDriftTime);
        // --> right
        o2::TPC::getHits(*simChains.get(), *context.get(), hitvectorsright, hitidsright,
                         getBranchNameRight(sector).c_str(), starttime, endtime, o2::TPC::calcDriftTime);

        LOG(DEBUG) << "DRIFTTIME " << drift << " SECTOR " << sector << " : SELECTED LEFT " << hitidsleft.size() << " IDs"
                   << " SELECTED RIGHT " << hitidsright.size();

        // invoke digitizer if anything to digitize within this drift interval
        if (hitidsleft.size() > 0 || hitidsright.size() > 0) {
          digitizertask->setData(&hitvectorsleft, &hitvectorsright, &hitidsleft, &hitidsright, context.get());
          digitizertask->setupSector(sector);
          digitizertask->Exec2("");

          std::copy(digitArrayRaw->begin(), digitArrayRaw->end(), std::back_inserter(digitAccum));
          labelAccum.mergeAtBack(*mcTruthArrayRaw);

          // NOTE: we would like to send it here in order to avoid copying/accumulating !!
        }
      }
      eventAccum.emplace_back(0, digitAccum.size()); // all digits are grouped to 1 super-event
    } else {                                         // pseudo-triggered mode
      for (int coll = 0; coll < numberofcollisions; coll++) {
        int startSize = digitAccum.size();
        digitizertask->setStartTime(0);
        digitizertask->setEndTime(2 * TPCDRIFT);
        hitidsleft.clear();
        hitidsright.clear();
        // obtain candidate hit(ids) for this collision --> left
        o2::TPC::getHits(*simChains.get(), *context.get(), hitvectorsleft, hitidsleft,
                         getBranchNameLeft(sector).c_str(), coll);
        // obtain candidate hit(ids) for this collision --> right
        o2::TPC::getHits(*simChains.get(), *context.get(), hitvectorsright, hitidsright,
                         getBranchNameRight(sector).c_str(), coll);
        int tmpND = 0;
        if (hitidsleft.size() > 0 || hitidsright.size() > 0) {
          digitizertask->setData(&hitvectorsleft, &hitvectorsright, &hitidsleft, &hitidsright, context.get());
          digitizertask->setupSector(sector);
          digitizertask->Exec2("");

          std::copy(digitArrayRaw->begin(), digitArrayRaw->end(), std::back_inserter(digitAccum));
          labelAccum.mergeAtBack(*mcTruthArrayRaw);
          // NOTE: we would like to send it here in order to avoid copying/accumulating !!
          tmpND = digitArrayRaw->size();
        }
        // register digits from this collision as a pseudo-trigger
        eventAccum.emplace_back(startSize, digitAccum.size() - startSize);
      }
    }
    // snapshot / "send" digits + MC truth
    snapshotEvents(eventAccum);
    snapshotDigits(digitAccum);
    snapshotLabels(labelAccum);
    timer.Stop();
    LOG(INFO) << "Digitization took " << timer.CpuTime() << "s";
  };

  // init function return a lambda taking a ProcessingContext
  auto initIt = [digitizertask, digitArray, mcTruthArray, simChain, simChains, doit](InitContext& ctx) {
    // Switch on distortions and get initial space-charge density histogram if provided in environment variables
    auto useDistortions = ctx.options().get<int>("distortionType");
    auto gridSizeString = ctx.options().get<std::string>("gridSize");
    auto triggeredMode = ctx.options().get<bool>("TPCtriggered");

    std::vector<int> gridSize;
    std::stringstream ss(gridSizeString);
    while (ss.good()) {
      std::string substr;
      getline(ss, substr, ',');
      gridSize.push_back(std::stoi(substr));
    }
    auto inputHistoString = ctx.options().get<std::string>("initialSpaceChargeDensity");
    std::vector<std::string> inputHisto;
    std::stringstream ssHisto(inputHistoString);
    while (ssHisto.good()) {
      std::string substr;
      getline(ssHisto, substr, ',');
      inputHisto.push_back(substr);
    }
    if (useDistortions > 0) {
      o2::TPC::SpaceCharge::SCDistortionType distortionType = useDistortions == 1 ? o2::TPC::SpaceCharge::SCDistortionType::SCDistortionsRealistic : o2::TPC::SpaceCharge::SCDistortionType::SCDistortionsConstant;
      std::unique_ptr<TH3> hisSCDensity;
      if (TString(inputHisto[0].data()).EndsWith(".root") && inputHisto[1].size() != 0) {
        auto fileSCInput = std::unique_ptr<TFile>(TFile::Open(inputHisto[0].data()));
        hisSCDensity.reset((TH3*)fileSCInput->Get(inputHisto[1].data()));
        hisSCDensity->SetDirectory(nullptr);
      }
      digitizertask->enableSCDistortions(distortionType, hisSCDensity.get(), gridSize[0], gridSize[1], gridSize[2]);
    }
    digitizertask->setContinuousReadout(!triggeredMode);

    digitizertask->Init2();
    // the task takes the ownership of digit array + mc truth array
    // TODO: make this clear in the API
    digitizertask->setOutputData(digitArray.get(), mcTruthArray.get());

    // setup the input chain
    simChains->emplace_back(new TChain("o2sim"));
    // add the background chain
    simChains->back()->AddFile(ctx.options().get<std::string>("simFile").c_str());
    auto signalfilename = ctx.options().get<std::string>("simFileS");
    if (signalfilename.size() > 0) {
      simChains->emplace_back(new TChain("o2sim"));
      simChains->back()->AddFile(signalfilename.c_str());
    }

    LOG(INFO) << "HAVE " << simChains->size() << " chains \n";

    simChain->AddFile(ctx.options().get<std::string>("simFile").c_str());
    return doit;
  };

  std::stringstream id;
  id << "TPCDigitizer" << channel;

  std::vector<OutputSpec> outputs; // define channel by triple of (origin, type id of data to be sent on this channel, subspecification)
  outputs.emplace_back("TPC", "DIGITS", static_cast<SubSpecificationType>(channel), Lifetime::Timeframe);
  outputs.emplace_back("TPC", "DIGTRIGGERS", static_cast<SubSpecificationType>(channel), Lifetime::Timeframe);
  outputs.emplace_back("TPC", "DIGITSMCTR", static_cast<SubSpecificationType>(channel), Lifetime::Timeframe);
  if (firstChannel == -1) {
    firstChannel = channel;
    outputs.emplace_back("TPC", "ROMode", 0, Lifetime::Timeframe);
    LOG(INFO) << " Channel " << channel << " will suply ROMode";
  }

  return DataProcessorSpec{
    id.str().c_str(), Inputs{ InputSpec{ "collisioncontext", "SIM", "COLLISIONCONTEXT", static_cast<SubSpecificationType>(channel), Lifetime::Timeframe } },
    outputs,
    AlgorithmSpec{ initIt },
    Options{ { "simFile", VariantType::String, "o2sim.root", { "Sim (background) input filename" } },
             { "simFileS", VariantType::String, "", { "Sim (signal) input filename" } },
             { "distortionType", VariantType::Int, 0, { "Distortion type to be used. 0 = no distortions (default), 1 = realistic distortions (not implemented yet), 2 = constant distortions" } },
             { "gridSize", VariantType::String, "33,180,33", { "Comma separated list of number of bins in z, phi and r for distortion lookup tables (z and r can only be 2**N + 1, N=1,2,3,...)" } },
             { "initialSpaceChargeDensity", VariantType::String, "", { "Path to root file containing TH3 with initial space-charge density and name of the TH3 (comma separated)" } },
             { "TPCtriggered", VariantType::Bool, false, { "Impose triggered RO mode (default: continuous)" } } }
  };
}
} // namespace steer
} // namespace o2
