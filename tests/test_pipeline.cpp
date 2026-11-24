#include "../Search/Graph.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int runCmd(const std::string& innerCmd) {
#ifdef _WIN32
    std::string wrapped = "\"" + innerCmd + "\"";
    return std::system(wrapped.c_str());
#else
    return std::system(innerCmd.c_str());
#endif
}

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    assert(static_cast<bool>(f));
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void writeText(const fs::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::binary);
    assert(static_cast<bool>(f));
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    assert(static_cast<bool>(f));
}

static void testGraphUnit(const fs::path& tmp) {
    Graph g;
    g.hour = 8;
    g.capLoad = 50.0;

    Edge e1;
    e1.srcId = 100; e1.dstId = 200; e1.routeId = 1; e1.routeName = "100"; e1.seq = 1;
    e1.travelTime = 60.0; e1.load = 10.0; e1.congestion = 0.2;
    g.addEdge(e1);

    // Unknown stop lookups should return empty vectors without throwing.
    assert(g.neighbors(999).empty());
    assert(g.edgesBetween(999, 200).empty());
    assert(g.routesAt(999).empty());
    assert(g.neighbors(100) == std::vector<int>({200}));
    assert(g.routesAt(200) == std::vector<std::string>({"100"}));

    // Negative / NaN travelTime edges must be ignored in shortestPath.
    Edge eBadNeg;
    eBadNeg.srcId = 100; eBadNeg.dstId = 200; eBadNeg.routeId = 2; eBadNeg.routeName = "BAD1";
    eBadNeg.travelTime = -10.0; eBadNeg.congestion = 0.0;
    g.addEdge(eBadNeg);

    Edge eBadNan;
    eBadNan.srcId = 100; eBadNan.dstId = 200; eBadNan.routeId = 3; eBadNan.routeName = "BAD2";
    eBadNan.travelTime = std::numeric_limits<double>::quiet_NaN(); eBadNan.congestion = 0.0;
    g.addEdge(eBadNan);

    PathResult pr = g.shortestPath(100, 200, 1.0);
    assert(pr.found());
    assert(std::abs(pr.totalTime - 60.0) < 1e-6);
    assert(std::abs(pr.cost - 72.0) < 1e-6);

    // Save / load roundtrip & trailing slash support in loadGraph.
    fs::path gDir = tmp / "graphs_roundtrip";
    fs::create_directories(gDir);
    fs::path binPath = gDir / "graph_08.bin";
    g.save(binPath.string());

    Graph gLoaded = loadGraph(8, gDir.string() + "/");
    assert(gLoaded.hour == 8);
    assert(gLoaded.numNodes() == 2);
    assert(gLoaded.numEdges() == 3);

    // Corrupted binary files must throw cleanly instead of UB/OOM.
    fs::path badMagic = tmp / "bad_magic.bin";
    writeText(badMagic, "NOPE");
    bool threw = false;
    try { Graph::load(badMagic.string()); } catch (const std::runtime_error&) { threw = true; }
    assert(threw);

    fs::path badNodeCnt = tmp / "bad_cnt.bin";
    {
        std::ofstream f(badNodeCnt, std::ios::binary);
        f.write("BUSG", 4);
        int32_t ver = 2, hr = 0, nn = -5;
        double cap = 1.0;
        f.write(reinterpret_cast<const char*>(&ver), 4);
        f.write(reinterpret_cast<const char*>(&hr), 4);
        f.write(reinterpret_cast<const char*>(&cap), 8);
        f.write(reinterpret_cast<const char*>(&nn), 4);
    }
    threw = false;
    try { Graph::load(badNodeCnt.string()); } catch (const std::runtime_error&) { threw = true; }
    assert(threw);

    fs::path badStrLen = tmp / "bad_strlen.bin";
    {
        std::ofstream f(badStrLen, std::ios::binary);
        f.write("BUSG", 4);
        int32_t ver = 2, hr = 0, nn = 2, n1 = 1, n2 = 2, ne = 1, rid = 1;
        uint32_t hugeLen = 0xFFFFFFFFu;
        double cap = 1.0;
        f.write(reinterpret_cast<const char*>(&ver), 4);
        f.write(reinterpret_cast<const char*>(&hr), 4);
        f.write(reinterpret_cast<const char*>(&cap), 8);
        f.write(reinterpret_cast<const char*>(&nn), 4);
        f.write(reinterpret_cast<const char*>(&n1), 4);
        f.write(reinterpret_cast<const char*>(&n2), 4);
        f.write(reinterpret_cast<const char*>(&ne), 4);
        f.write(reinterpret_cast<const char*>(&n1), 4);
        f.write(reinterpret_cast<const char*>(&n2), 4);
        f.write(reinterpret_cast<const char*>(&rid), 4);
        f.write(reinterpret_cast<const char*>(&hugeLen), 4);
    }
    threw = false;
    try { Graph::load(badStrLen.string()); } catch (const std::runtime_error&) { threw = true; }
    assert(threw);

    std::cout << "[PASS] Graph unit tests\n";
}

static void testBusEdgeAndCongestionCli(const fs::path& tmp, const fs::path& binDir) {
    fs::path busDir = tmp / "bus_edge_case";
    fs::create_directories(busDir);

    writeText(busDir / fs::u8path("서울시버스노선ID정보(20260902).csv"),
              "\xEF\xBB\xBFROUTEID,노선명\r\n1,040\r\n2,\"100,A\"\r\n");

    writeText(busDir / fs::u8path("서울시버스노선별정류소정보(20260902).csv"),
              "\xEF\xBB\xBFROUTE_ID,노선명,NODE_ID,순번,정류소명,X좌표,Y좌표\r\n"
              "1,040,10,1,StopA,127.0,37.5\r\n"
              "1,040,20,2,StopB,127.1,37.6\r\n"
              "1,040,30,3,StopC,127.2,37.7\r\n");

    std::string timeHeader = "기준_날짜,노선_ID,출발_정류장_ID,도착_정류장_ID,출발_정류장_순서,도착_정류장_순서,운행시간";
    for (int h = 0; h < 24; ++h) {
        char buf[32];
        std::snprintf(buf, sizeof buf, ",운행시간_%02d시", h);
        timeHeader += buf;
    }
    std::string timeRow1 = "20260914,1,10,20,01,02,120";
    for (int h = 0; h < 24; ++h) timeRow1 += ",120";
    std::string timeRow2 = "20260914,1,20,30,02,03,0";
    for (int h = 0; h < 24; ++h) timeRow2 += ",60";

    writeText(busDir / fs::u8path("tpss_route_section_speedh_2026.09.14-09.20.csv"),
              timeHeader + "\r\n" + timeRow1 + "\r\n" + timeRow2 + "\r\n");

    std::string busEdgeExe = (binDir / "bus_edge.exe").string();
    std::string cmd = "\"" + busEdgeExe + "\" \"" + busDir.string() + "\"";
    int rc = runCmd(cmd);
    assert(rc == 0);

    std::string outCsv = readFile(busDir / "bus_edges.csv");
    // Sequence "01"->"02" should normalize to "1"->"2" and match topologyEdges (no duplicate row).
    int lineCount = 0;
    for (char c : outCsv) if (c == '\n') ++lineCount;
    assert(lineCount == 3); // header + 2 edge rows

    // Verify normalizeTime rejects "inf".
    fs::path badDir = tmp / "bus_edge_bad";
    fs::create_directories(badDir);
    fs::copy_file(busDir / fs::u8path("서울시버스노선ID정보(20260902).csv"),
                  badDir / fs::u8path("서울시버스노선ID정보(20260902).csv"),
                  fs::copy_options::overwrite_existing);
    fs::copy_file(busDir / fs::u8path("서울시버스노선별정류소정보(20260902).csv"),
                  badDir / fs::u8path("서울시버스노선별정류소정보(20260902).csv"),
                  fs::copy_options::overwrite_existing);
    std::string badRow = "20260914,1,10,20,1,2,inf";
    for (int h = 0; h < 24; ++h) badRow += ",10";
    writeText(badDir / fs::u8path("tpss_route_section_speedh_2026.09.14-09.20.csv"),
              timeHeader + "\r\n" + badRow + "\r\n");
    std::string badCmd = "\"" + busEdgeExe + "\" \"" + badDir.string() + "\" 2>nul";
    int badRc = runCmd(badCmd);
    assert(badRc != 0);

    // Test congestion.cpp directly against bus_edge.cpp output (이동시간_s) + 2-digit hour headers + empty ridership fallback.
    std::string rideHeader = "사용년월,노선번호,표준버스정류장ID";
    for (int h = 0; h < 24; ++h) {
        char buf[128];
        std::snprintf(buf, sizeof buf, ",%02d시승차총승객수,%02d시하차총승객수", h, h);
        rideHeader += buf;
    }
    std::string rideRow1 = "202609,040,10";
    std::string rideRow2 = "202609,040,20";
    std::string rideRow3 = "202609,040,30";
    for (int h = 0; h < 24; ++h) {
        rideRow1 += ",300,0";    // 10명/day boarding at stop 10
        rideRow2 += ",0,600";    // over-alighting at stop 20 (must clamp cum to 0, not negative)
        rideRow3 += ",300,0";    // boarding at stop 30 should still show positive load if continued
    }
    fs::path rideCsv = busDir / "ridership.csv";
    writeText(rideCsv, rideHeader + "\r\n" + rideRow1 + "\r\n" + rideRow2 + "\r\n" + rideRow3 + "\r\n");

    std::string congExe = (binDir / "congestion.exe").string();
    fs::path congOut = busDir / "congestion.csv";
    std::string congCmd = "\"" + congExe + "\" \"" + (busDir / "bus_edges.csv").string() + "\" \"" +
                          rideCsv.string() + "\" \"" + congOut.string() + "\"";
    assert(runCmd(congCmd) == 0);
    std::string congData = readFile(congOut);
    assert(congData.find("nan") == std::string::npos);
    assert(congData.find("1,040,10,20,1,1.0000") != std::string::npos);

    // Test empty ridership (vals.empty() previously caused size_t underflow UB in percentile).
    fs::path emptyRideCsv = busDir / "ridership_empty.csv";
    writeText(emptyRideCsv, rideHeader + "\r\n");
    fs::path congEmptyOut = busDir / "congestion_empty.csv";
    std::string congEmptyCmd = "\"" + congExe + "\" \"" + (busDir / "bus_edges.csv").string() + "\" \"" +
                               emptyRideCsv.string() + "\" \"" + congEmptyOut.string() + "\"";
    assert(runCmd(congEmptyCmd) == 0);

    std::cout << "[PASS] bus_edge & congestion CLI tests\n";
}

static void testPathfinderCli(const fs::path& tmp, const fs::path& binDir) {
    // Build 24 synthetic graphs to verify cross-hour state-space search:
    // Departure 07:59 (hour 7, min 59).
    // Route A: 1 -> 2 takes 120s (arrives 08:01, hour 8) with cost 120.
    // Route B: 1 -> 2 takes 30s  (arrives 07:59:30, hour 7) with cost 30.
    // From stop 2 -> 3:
    //   In hour 7: travelTime = 1000s (high congestion/slow)
    //   In hour 8: travelTime = 50s   (fast!)
    // A single-label-per-node Dijkstra prunes Route A at stop 2 because 120 > 30, missing 1->2->3 total cost 170!
    // The (node, hour) state-space Dijkstra finds Route A (120 + 50 = 170) instead of Route B (30 + 1000 = 1030).
    fs::path gDir = tmp / "pf_graphs";
    fs::create_directories(gDir);
    for (int h = 0; h < 24; ++h) {
        Graph g;
        g.hour = h;
        g.capLoad = 100.0;

        Edge eA;
        eA.srcId = 1; eA.dstId = 2; eA.routeId = 10; eA.routeName = "A";
        eA.travelTime = 120.0; eA.congestion = 0.0; eA.load = 0.0;
        g.addEdge(eA);

        Edge eB;
        eB.srcId = 1; eB.dstId = 2; eB.routeId = 20; eB.routeName = "B";
        eB.travelTime = 30.0; eB.congestion = 0.0; eB.load = 0.0;
        g.addEdge(eB);

        Edge e23;
        e23.srcId = 2; e23.dstId = 3; e23.routeId = 30; e23.routeName = "C";
        e23.travelTime = (h == 8) ? 50.0 : 1000.0;
        e23.congestion = 0.0; e23.load = 0.0;
        g.addEdge(e23);

        char name[32];
        std::snprintf(name, sizeof name, "graph_%02d.bin", h);
        g.save((gDir / name).string());
    }

    fs::path workDir = tmp / "pf_work";
    fs::create_directories(workDir);
    fs::path inFile = workDir / "input.txt";
    writeText(inFile, "7 59 1 3\n-1\n");

    std::string pfExe = (binDir / "pathfinder.exe").string();
    std::string cmd = "cd /d \"" + workDir.string() + "\" && \"" + pfExe + "\" \"" + gDir.string() +
                      "\" 1.0 nonexistent_stops.csv < \"" + inFile.string() + "\" > \"" +
                      (workDir / "out.txt").string() + "\"";
    assert(runCmd(cmd) == 0);

    std::string pfCsv = readFile(workDir / "path_results.csv");
    // Expect cost 120 + 50 = 170 (Route A in hour 7 then Route C in hour 8)
    assert(pfCsv.find("7,0,1,10,A,1,,2,,120") != std::string::npos);
    assert(pfCsv.find("8,0,2,30,C,2,,3,,50") != std::string::npos);

    std::cout << "[PASS] Pathfinder cross-hour CLI tests\n";
}

int main(int argc, char* argv[]) {
    assert(argc >= 2);
    fs::path binDir = fs::path(argv[1]);
    fs::path tmp = fs::temp_directory_path() / "kwu_ds_06_tests";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    testGraphUnit(tmp);
    testBusEdgeAndCongestionCli(tmp, binDir);
    testPathfinderCli(tmp, binDir);

    fs::remove_all(tmp);
    std::cout << "ALL TESTS PASSED\n";
    return 0;
}
