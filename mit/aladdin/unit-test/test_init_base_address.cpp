#include "catch.hpp"
#include "DDDG.h"
#include "file_func.h"
#include "Scratchpad.h"
#include "ScratchpadDatapath.h"

SCENARIO("Test initBaseAddress w/ c[i]=a[i] case", "[equal]") {
  GIVEN("Test c[i]=a[i] case w/ Input Size 32") {
    std::string bench("outputs/triad-initbase");
    std::string trace_file("inputs/triad-initbase-trace.gz");
    std::string config_file("inputs/config-triad-initbase-p1-u1-P1");

    ScratchpadDatapath* acc;
    Scratchpad* spad;
    acc = new ScratchpadDatapath(bench, trace_file, config_file);
    auto& prog = acc->getProgram();
    acc->buildDddg();
    acc->removeInductionDependence();
    acc->removePhiNodes();
    WHEN("Test initBaseAddress()") {
      acc->initBaseAddress();
      THEN("The baseAddress of memory operations should be either "
           "'a', 'b' or 'c' for Triad.") {
        REQUIRE(prog.getBaseAddressLabel(3).compare("a") == 0);
        REQUIRE(prog.getBaseAddressLabel(5).compare("c") == 0);
        REQUIRE(prog.getBaseAddressLabel(11).compare("a") == 0);
        REQUIRE(prog.getBaseAddressLabel(13).compare("c") == 0);
        REQUIRE(prog.getBaseAddressLabel(19).compare("a") == 0);
        REQUIRE(prog.getBaseAddressLabel(21).compare("c") == 0);
      }
    }
  }
}

SCENARIO("Test initBaseAddress w/ Triad", "[triad]") {
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
    acc->removePhiNodes();
    WHEN("Test initBaseAddress()") {
      acc->initBaseAddress();
      THEN("The baseAddress of memory operations should be either "
           "'a', 'b' or 'c' for Triad.") {
        REQUIRE(prog.getBaseAddressLabel(3).compare("a") == 0);
        REQUIRE(prog.getBaseAddressLabel(5).compare("b") == 0);
        REQUIRE(prog.getBaseAddressLabel(9).compare("c") == 0);
        REQUIRE(prog.getBaseAddressLabel(1491).compare("a") == 0);
        REQUIRE(prog.getBaseAddressLabel(1493).compare("b") == 0);
        REQUIRE(prog.getBaseAddressLabel(1497).compare("c") == 0);
      }
    }
  }
}
SCENARIO("Test initBaseAddress w/ Reduction", "[reduction]") {
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
    acc->removePhiNodes();
    WHEN("Test initBaseAddress()") {
      acc->initBaseAddress();
      THEN("The baseAddress of memory operations should be 'in' for "
           "Reduction.") {
        REQUIRE(prog.getBaseAddressLabel(4).compare("in") == 0);
        REQUIRE(prog.getBaseAddressLabel(12).compare("in") == 0);
        REQUIRE(prog.getBaseAddressLabel(1020).compare("in") == 0);
      }
    }
  }
}
SCENARIO("Test initBaseAddress w/ Pp_scan", "[pp_scan]") {
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
    acc->removePhiNodes();
    WHEN("Test initBaseAddress()") {
      acc->initBaseAddress();
      THEN("The baseAddress of memory operations should be 'bucket' or 'sum' "
           "for pp_scan.") {
        REQUIRE(prog.getBaseAddressLabel(11).compare("bucket") == 0);
        REQUIRE(prog.getBaseAddressLabel(13).compare("bucket") == 0);
        REQUIRE(prog.getBaseAddressLabel(15).compare("bucket") == 0);
        REQUIRE(prog.getBaseAddressLabel(1463).compare("bucket") == 0);
        REQUIRE(prog.getBaseAddressLabel(1465).compare("bucket") == 0);
        REQUIRE(prog.getBaseAddressLabel(1467).compare("bucket") == 0);
        REQUIRE(prog.getBaseAddressLabel(1488).compare("bucket") == 0);
        REQUIRE(prog.getBaseAddressLabel(1491).compare("sum") == 0);
        REQUIRE(prog.getBaseAddressLabel(3169).compare("bucket") == 0);
        REQUIRE(prog.getBaseAddressLabel(3167).compare("sum") == 0);
      }
    }
  }
}

SCENARIO("Test initBaseAddress w/ sort-radix", "[sort-radix]") {
  GIVEN("Test the mapping between global variables to function's local variables.") {
    std::string bench("outputs/sort-radix");
    std::string trace_file("inputs/sort-radix-trace.gz");
    std::string config_file("inputs/config-sort-radix");

    ScratchpadDatapath* acc;
    Scratchpad* spad;
    acc = new ScratchpadDatapath(bench, trace_file, config_file);
    auto& prog = acc->getProgram();
    acc->buildDddg();
    acc->removeInductionDependence();
    acc->removePhiNodes();
    WHEN("Test initBaseAddress()") {
      acc->initBaseAddress();
      THEN("In the 1st update(): global b -> local b, global a -> local a.\n\
           In the 2nd update(): global a -> local b, global b -> local a.\n\
           In the 3rd update(): global b -> local b, global a -> local a.") {
        REQUIRE(prog.getBaseAddressLabel(97169).compare("a") == 0);
        REQUIRE(prog.getBaseAddressLabel(97180).compare("b") == 0);
        REQUIRE(prog.getBaseAddressLabel(240417).compare("b") == 0);
        REQUIRE(prog.getBaseAddressLabel(240428).compare("a") == 0);
        REQUIRE(prog.getBaseAddressLabel(383665).compare("a") == 0);
        REQUIRE(prog.getBaseAddressLabel(383676).compare("b") == 0);
      }
    }
  }
}
