#include "catch.hpp"
#include "DDDG.h"
#include "file_func.h"
#include "Scratchpad.h"
#include "ScratchpadDatapath.h"

SCENARIO("Test removePhiNodes w/ Triad", "[triad]") {
  GIVEN("Test Triad w/ Input Size 128") {
    std::string bench("outputs/triad-128");
    std::string trace_file("inputs/triad-128-trace.gz");
    std::string config_file("inputs/config-triad-p2-u2-P1");

    ScratchpadDatapath* acc;
    Scratchpad* spad;
    acc = new ScratchpadDatapath(bench, trace_file, config_file);
    auto& prog = acc->getProgram();
    acc->buildDddg();
    acc->removeInductionDependence();
    WHEN("Test removePhiNodes()") {
      acc->removePhiNodes();
      THEN("Phi Nodes in the DDDG should be isolated.") {
        REQUIRE(prog.getNumConnectedNodes(13) == 0);
        REQUIRE(prog.getNumConnectedNodes(25) == 0);
        REQUIRE(prog.getNumConnectedNodes(1525) == 0);
      }
    }
  }
}
SCENARIO("Test removePhiNodes w/ Reduction", "[reduction]") {
  GIVEN("Test Reduction w/ Input Size 128") {
    std::string bench("outputs/reduction-128");
    std::string trace_file("inputs/reduction-128-trace.gz");
    std::string config_file("inputs/config-reduction-p4-u4-P1");

    ScratchpadDatapath* acc;
    Scratchpad* spad;
    acc = new ScratchpadDatapath(bench, trace_file, config_file);
    auto& prog = acc->getProgram();
    acc->buildDddg();
    acc->removeInductionDependence();
    WHEN("Test removePhiNodes()") {
      acc->removePhiNodes();
      THEN("Phi Nodes in the DDDG should be isolated.") {
        REQUIRE(prog.getNumConnectedNodes(1) == 0);
        REQUIRE(prog.getNumConnectedNodes(10) == 0);
        REQUIRE(prog.getNumConnectedNodes(1009) == 0);
        REQUIRE(prog.getNumConnectedNodes(1018) == 0);
      }
    }
  }
}
SCENARIO("Test removePhiNodes w/ Pp_scan", "[pp_scan]") {
  GIVEN("Test Pp_scan w/ Input Size 128") {
    std::string bench("outputs/pp_scan-128");
    std::string trace_file("inputs/pp_scan-128-trace.gz");
    std::string config_file("inputs/config-pp_scan-p4-u4-P1");

    ScratchpadDatapath* acc;
    Scratchpad* spad;
    acc = new ScratchpadDatapath(bench, trace_file, config_file);
    auto& prog = acc->getProgram();
    acc->buildDddg();
    acc->removeInductionDependence();
    WHEN("Test removePhiNodes()") {
      acc->removePhiNodes();
      THEN("Phi Nodes in the DDDG should be isolated.") {
        REQUIRE(prog.getNumConnectedNodes(2) == 0);
        REQUIRE(prog.getNumConnectedNodes(19) == 0);
        REQUIRE(prog.getNumConnectedNodes(1478) == 0);
        REQUIRE(prog.getNumConnectedNodes(1495) == 0);
        REQUIRE(prog.getNumConnectedNodes(1736) == 0);
        REQUIRE(prog.getNumConnectedNodes(3163) == 0);
      }
    }
  }
}
