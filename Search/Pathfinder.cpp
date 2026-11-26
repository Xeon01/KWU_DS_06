#include "Graph.hpp"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

// 쉼표, 따옴표, 줄바꿈이 포함된 CSV 필드를 RFC 4180 형식으로 이스케이프한다.
std::string csvEscape(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) {
        return value;
    }
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"') out += "\"\"";
        else out += ch;
    }
    out += '"';
    return out;
}

// 한 줄의 CSV 레코드를 파싱한다 (따옴표 내 쉼표 및 "" 이스케이프 지원).
ds::Vector<std::string> parseCsvLine(const std::string& line) {
    ds::Vector<std::string> row;
    std::string field;
    bool inQuotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char ch = line[i];
        if (inQuotes) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += ch;
            }
        } else {
            if (ch == '"' && field.empty()) {
                inQuotes = true;
            } else if (ch == ',') {
                row.push_back(field);
                field.clear();
            } else if (ch != '\r') {
                field += ch;
            }
        }
    }
    row.push_back(field);
    return row;
}

// 문자열 전체가 int 범위의 10진 정수인지 안전하게 파싱한다.
bool tryParseInt(const std::string& token, int& out) {
    if (token.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long val = std::strtol(token.c_str(), &end, 10);
    if (errno == ERANGE || end == token.c_str() || *end != '\0') return false;
    if (val < INT_MIN || val > INT_MAX) return false;
    out = static_cast<int>(val);
    return true;
}

// 문자열 경로를 안전하게 fs::path로 변환한다 (Windows MinGW의 C 로캘 EILSEQ 우회).
fs::path toFsPath(const std::string& p) {
#ifdef _WIN32
    if (p.empty()) return fs::path();
    auto convert = [&](UINT cp, DWORD flags) -> std::wstring {
        int len = MultiByteToWideChar(cp, flags, p.data(), static_cast<int>(p.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring w(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(cp, flags, p.data(), static_cast<int>(p.size()), &w[0], len);
        return w;
    };
    std::wstring w = convert(CP_UTF8, MB_ERR_INVALID_CHARS);
    if (w.empty()) w = convert(CP_ACP, 0);
    if (!w.empty()) return fs::path(w);
#endif
    return fs::path(p);
}

// 콘솔(UTF-8) 출력용으로 경로 문자열을 변환한다.
std::string pathToUtf8(const std::string& p) {
#ifdef _WIN32
    std::wstring w = toFsPath(p).wstring();
    if (w.empty()) return p;
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return p;
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], len, nullptr, nullptr);
    return out;
#else
    return p;
#endif
}

// Graph::load(const std::string&)에 전달할 경로 문자열을 결합한다.
std::string joinPathString(const std::string& folder, const std::string& file) {
    if (folder.empty()) return file;
    char back = folder.back();
    if (back == '/' || back == '\\') return folder + file;
    return folder + "/" + file;
}

// 기본 그래프 폴더를 탐색한다 (실행 위치에 따라 graphs 또는 Search/graphs 허용).
std::string resolveDefaultGraphFolder() {
    std::error_code ec;
    if (fs::exists(toFsPath("graphs"), ec) && fs::is_directory(toFsPath("graphs"), ec)) return "graphs";
    if (fs::exists(toFsPath("Search/graphs"), ec) && fs::is_directory(toFsPath("Search/graphs"), ec)) {
        return "Search/graphs";
    }
    return "graphs";
}

// 00:00:00 기준 경과 초를 "HH:MM" (24시간 주기) 문자열로 포맷한다.
std::string formatClock(long long totalSec) {
    long long daySec = ((totalSec % 86400) + 86400) % 86400;
    int hh = static_cast<int>(daySec / 3600);
    int mm = static_cast<int>((daySec % 3600) / 60);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
    return buf;
}

// 경과 초를 기준으로 현재 사용할 그래프 시간대(0~23)를 반환한다 (예: 07:59 -> 7, 08:00 -> 8).
int hourFromWallSec(long long wallSec) {
    long long daySec = ((wallSec % 86400) + 86400) % 86400;
    return static_cast<int>(daySec / 3600);
}

}  // namespace

// 24개 시간대 그래프의 정류장 ID를 통합 인덱스로 매핑한다.
struct UnifiedStopIndex {
    ds::HashMap<int, int> stopToDense;
    ds::Vector<int> denseToStop;
    ds::Vector<ds::Array<int, 24>> nodeIdxInHour;

    void build(const ds::Vector<Graph>& graphs) {
        stopToDense.clear();
        denseToStop.clear();
        nodeIdxInHour.clear();

        for (int h = 0; h < 24; ++h) {
            for (int ni = 0; ni < graphs[h].numNodes(); ++ni) {
                int stopId = graphs[h].nodeAt(ni).id;
                auto it = stopToDense.find(stopId);
                int dense = -1;
                if (it == stopToDense.end()) {
                    dense = static_cast<int>(denseToStop.size());
                    stopToDense[stopId] = dense;
                    denseToStop.push_back(stopId);
                    ds::Array<int, 24> arr;
                    arr.fill(-1);
                    nodeIdxInHour.push_back(arr);
                } else {
                    dense = it->second;
                }
                nodeIdxInHour[dense][h] = ni;
            }
        }
    }

    int denseOf(int stopId) const {
        auto it = stopToDense.find(stopId);
        return (it == stopToDense.end()) ? -1 : it->second;
    }

    bool hasInAnyHour(int stopId) const { return denseOf(stopId) >= 0; }
};

// 정류장 ID <-> 이름 매핑 구조체 (stops.csv가 있을 때 활성화, 없으면 ID만 사용)
struct StopDirectory {
    ds::HashMap<int, std::string> idToName;
    ds::HashMap<std::string, ds::Vector<int>> nameToIds;

    void loadCsv(const std::string& csvPath) {
        std::ifstream f(toFsPath(csvPath), std::ios::binary);
        if (!f) return;

        std::string line;
        bool isFirst = true;
        while (std::getline(f, line)) {
            if (isFirst) {
                isFirst = false;
                if (line.rfind("\xEF\xBB\xBF", 0) == 0) line = line.substr(3);
                if (line.empty() || line == "\r") continue;
                ds::Vector<std::string> cols = parseCsvLine(line);
                int id = 0;
                if (cols.empty() || !tryParseInt(cols[0], id)) continue;  // 헤더 스킵
                if (cols.size() >= 2) {
                    const std::string& name = cols[1];
                    idToName[id] = name;
                    auto& ids = nameToIds[name];
                    if (ds::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
                }
                continue;
            }
            if (line.empty() || line == "\r") continue;
            ds::Vector<std::string> cols = parseCsvLine(line);
            if (cols.size() < 2) continue;

            int id = 0;
            if (!tryParseInt(cols[0], id)) continue;
            const std::string& name = cols[1];

            idToName[id] = name;
            auto& ids = nameToIds[name];
            if (ds::find(ids.begin(), ids.end(), id) == ids.end()) {
                ids.push_back(id);
            }
        }
    }

    // 입력이 정수면 해당 ID 단일 목록을, 이름이면 매칭되는 모든 정류장 ID 목록을 반환한다.
    ds::Vector<int> resolveAll(const std::string& token) const {
        int id = 0;
        if (tryParseInt(token, id)) {
            ds::Vector<int> single;
            single.push_back(id);
            return single;
        }
        auto it = nameToIds.find(token);
        if (it != nameToIds.end()) return it->second;
        return {};
    }

    // 시작 시간대(또는 전체 시간대) 그래프에 존재하는 정류장 ID를 우선 선택하고, 동명 정류장이 여러 개면 안내한다.
    int pickStopId(const ds::Vector<Graph>& graphs, const UnifiedStopIndex& idx,
                   int startHour, const std::string& token, const char* roleLabel,
                   bool allowAnyHour) const {
        ds::Vector<int> candidates = resolveAll(token);
        ds::Vector<int> inGraph;
        for (int id : candidates) {
            if (graphs[startHour].hasNode(id)) inGraph.push_back(id);
        }
        if (inGraph.empty() && allowAnyHour) {
            for (int id : candidates) {
                if (idx.hasInAnyHour(id)) inGraph.push_back(id);
            }
        }
        if (inGraph.empty()) return -1;

        if (inGraph.size() > 1) {
            std::cout << "  [안내] " << roleLabel << " '" << token
                      << "'에 해당하는 정류장이 그래프에 " << inGraph.size() << "개 있습니다 (";
            for (size_t i = 0; i < inGraph.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << inGraph[i];
            }
            std::cout << ") -> 첫 번째(" << inGraph[0] << ") 사용 (ID 직접 입력 가능)\n";
        }
        return inGraph[0];
    }

    std::string formatStop(int id) const {
        auto it = idToName.find(id);
        if (it != idToName.end() && !it->second.empty()) {
            return it->second + "(" + std::to_string(id) + ")";
        }
        return std::to_string(id);
    }
};

struct TimeStep {
    int hour = -1;       // 이 구간에서 참조한 그래프 시간대(0~23)
    int edgeIdx = -1;    // graphs[hour].edges 내 간선 인덱스
    long long depSec = 0;
    long long arrSec = 0;
};

struct TimePathResult {
    double cost = -1;
    double totalTime = 0;
    ds::Vector<TimeStep> steps;
    bool found() const { return cost >= 0; }
};

struct TimeHeapItem {
    double cost;
    double elapsed;
    int dense;
    int hour;

    bool operator<(const TimeHeapItem& o) const {
        if (cost != o.cost) return cost < o.cost;
        if (elapsed != o.elapsed) return elapsed < o.elapsed;
        if (dense != o.dense) return dense < o.dense;
        return hour < o.hour;
    }
};

// 출발 시각(startHour:startMinute)부터 누적 소요시간에 따라 시간대별 그래프를 일괄 전환하며 최단 경로를 탐색한다.
// 상태 공간은 (정류장 인덱스, 도착 시간대 0~23)으로 확장하여 시간대 경계 전후의 비-FIFO 전이를 반영한다.
TimePathResult shortestPathTimeDependent(const ds::Vector<Graph>& graphs,
                                           const UnifiedStopIndex& idx,
                                           int srcId, int dstId,
                                           int startHour, int startMinute,
                                           double alpha,
                                           bool extremeCongestion = false) {
    TimePathResult result;
    int s = idx.denseOf(srcId);
    int t = idx.denseOf(dstId);
    if (s < 0 || t < 0) return result;
    if (startHour < 0 || startHour >= 24) return result;

    const long long startSec =
        static_cast<long long>(startHour) * 3600LL + static_cast<long long>(startMinute) * 60LL;

    if (s == t) {
        result.cost = 0.0;
        result.totalTime = 0.0;
        return result;
    }

    const size_t N = idx.denseToStop.size();
    const size_t S = N * 24;
    auto stateIdx = [](int dense, int h) -> size_t {
        return static_cast<size_t>(dense) * 24 + static_cast<size_t>(h);
    };

    const double INF = std::numeric_limits<double>::infinity();
    ds::Vector<double> bestCost(S, INF);
    ds::Vector<double> bestElapsed(S, INF);
    ds::Vector<int> prevDense(S, -1);
    ds::Vector<int> prevHour(S, -1);
    ds::Vector<int> prevEdge(S, -1);
    ds::Vector<double> prevStepDep(S, 0.0);

    // (누적 비용, 누적 소요시간, 통합 정류장 인덱스, 현재 시간대) 최소 힙
    ds::PriorityQueue<TimeHeapItem, ds::Greater<TimeHeapItem>> pq;

    size_t startState = stateIdx(s, startHour);
    bestCost[startState] = 0.0;
    bestElapsed[startState] = 0.0;
    pq.emplace(TimeHeapItem{0.0, 0.0, s, startHour});

    int endHour = -1;
    while (!pq.empty()) {
        TimeHeapItem top = pq.top();
        pq.pop();
        double d = top.cost;
        double elapsed = top.elapsed;
        int u = top.dense;
        int curHour = top.hour;

        size_t uh = stateIdx(u, curHour);
        if (d > bestCost[uh] || (d == bestCost[uh] && elapsed > bestElapsed[uh])) continue;
        if (u == t) {
            endHour = curHour;
            break;
        }

        int uInHour = idx.nodeIdxInHour[u][curHour];
        if (uInHour < 0) continue;

        const Graph& gCur = graphs[curHour];
        for (int ei : gCur.nodeAt(uInHour).out) {
            const Edge& e = gCur.edge(ei);
            if (!e.hasData()) continue;  // 미운행/혼잡도 미집계 간선은 탐색하지 않음
            if (!std::isfinite(e.travelTime) || e.travelTime < 0.0) continue;
            double stepCost = extremeCongestion ? e.extremeCost() : e.cost(alpha);
            if (!std::isfinite(stepCost) || stepCost < 0.0) continue;

            int v = idx.denseOf(e.dstId);
            if (v < 0) continue;

            double nd = d + stepCost;
            double nt = elapsed + e.travelTime;
            int nextHour = hourFromWallSec(startSec + static_cast<long long>(std::floor(nt)));
            size_t vh = stateIdx(v, nextHour);
            if (nd < bestCost[vh] || (nd == bestCost[vh] && nt < bestElapsed[vh])) {
                bestCost[vh] = nd;
                bestElapsed[vh] = nt;
                prevDense[vh] = u;
                prevHour[vh] = curHour;
                prevEdge[vh] = ei;
                prevStepDep[vh] = elapsed;
                pq.emplace(TimeHeapItem{nd, nt, v, nextHour});
            }
        }
    }

    if (endHour < 0) return result;

    size_t endState = stateIdx(t, endHour);
    result.cost = bestCost[endState];
    result.totalTime = bestElapsed[endState];

    int cur = t;
    int curH = endHour;
    while (!(cur == s && curH == startHour)) {
        size_t ch = stateIdx(cur, curH);
        TimeStep step;
        step.hour = prevHour[ch];
        step.edgeIdx = prevEdge[ch];
        step.depSec = startSec + static_cast<long long>(std::floor(prevStepDep[ch]));
        step.arrSec = startSec + static_cast<long long>(std::floor(bestElapsed[ch]));
        result.steps.push_back(step);
        int nextCur = prevDense[ch];
        int nextH = prevHour[ch];
        cur = nextCur;
        curH = nextH;
    }
    ds::reverse(result.steps.begin(), result.steps.end());
    return result;
}

// srcId에서 출발하여 dstIds 중 시간대 전환을 반영해 도달 가능한 도착 정류장 ID 목록을 반환한다.
ds::Vector<int> findReachableTargetsTimeDependent(const ds::Vector<Graph>& graphs,
                                                  const UnifiedStopIndex& idx,
                                                  int srcId,
                                                  const ds::Vector<int>& dstIds,
                                                  int startHour, int startMinute) {
    ds::Vector<int> reachedDstIds;
    if (startHour < 0 || startHour >= 24) return reachedDstIds;
    if (!graphs[startHour].hasNode(srcId)) return reachedDstIds;

    int s = idx.denseOf(srcId);
    if (s < 0) return reachedDstIds;

    const size_t N = idx.denseToStop.size();
    ds::Vector<char> isTarget(N, 0);
    ds::Vector<char> targetReached(N, 0);
    size_t uniqueTargets = 0;
    for (int dstId : dstIds) {
        int t = idx.denseOf(dstId);
        if (t >= 0 && !isTarget[t]) {
            isTarget[t] = 1;
            ++uniqueTargets;
        }
    }
    if (uniqueTargets == 0) return reachedDstIds;

    const long long startSec =
        static_cast<long long>(startHour) * 3600LL + static_cast<long long>(startMinute) * 60LL;
    const size_t S = N * 24;
    auto stateIdx = [](int dense, int h) -> size_t {
        return static_cast<size_t>(dense) * 24 + static_cast<size_t>(h);
    };

    const double INF = std::numeric_limits<double>::infinity();
    ds::Vector<double> bestElapsed(S, INF);
    ds::PriorityQueue<TimeHeapItem, ds::Greater<TimeHeapItem>> pq;

    size_t startState = stateIdx(s, startHour);
    bestElapsed[startState] = 0.0;
    pq.emplace(TimeHeapItem{0.0, 0.0, s, startHour});

    size_t reachedCount = 0;
    while (!pq.empty()) {
        TimeHeapItem top = pq.top();
        pq.pop();
        double elapsed = top.elapsed;
        int u = top.dense;
        int curHour = top.hour;

        size_t uh = stateIdx(u, curHour);
        if (elapsed > bestElapsed[uh]) continue;

        if (isTarget[u] && !targetReached[u]) {
            targetReached[u] = 1;
            ++reachedCount;
            if (reachedCount == uniqueTargets) break;
        }

        int uInHour = idx.nodeIdxInHour[u][curHour];
        if (uInHour < 0) continue;

        const Graph& gCur = graphs[curHour];
        for (int ei : gCur.nodeAt(uInHour).out) {
            const Edge& e = gCur.edge(ei);
            if (!e.hasData()) continue;
            if (!std::isfinite(e.travelTime) || e.travelTime < 0.0) continue;

            int v = idx.denseOf(e.dstId);
            if (v < 0) continue;

            double nt = elapsed + e.travelTime;
            int nextHour = hourFromWallSec(startSec + static_cast<long long>(std::floor(nt)));
            size_t vh = stateIdx(v, nextHour);
            if (nt < bestElapsed[vh]) {
                bestElapsed[vh] = nt;
                pq.emplace(TimeHeapItem{nt, nt, v, nextHour});
            }
        }
    }

    for (int dstId : dstIds) {
        int t = idx.denseOf(dstId);
        if (t >= 0 && targetReached[t]) {
            reachedDstIds.push_back(dstId);
        }
    }
    return reachedDstIds;
}

// 00~23시 그래프 24개 로드 (graph_XX.bin 및 graphXX.bin 모두 지원)
ds::Vector<Graph> loadAllGraphs(const std::string& folder) {
    ds::Vector<Graph> graphs(24);

    for (int h = 0; h < 24; ++h) {
        char f1[32], f2[32];
        std::snprintf(f1, sizeof(f1), "graph_%02d.bin", h);
        std::snprintf(f2, sizeof(f2), "graph%02d.bin", h);

        std::string p1 = joinPathString(folder, f1);
        std::string p2 = joinPathString(folder, f2);
        std::error_code ec;

        if (fs::exists(toFsPath(p1), ec)) {
            graphs[h] = Graph::load(p1);
        } else if (fs::exists(toFsPath(p2), ec)) {
            graphs[h] = Graph::load(p2);
        } else {
            throw std::runtime_error("그래프 파일 없음: " + p1);
        }
    }
    return graphs;
}

// 두 정류장 기준 시간 의존 탐색 결과 출력
void printPath(const ds::Vector<Graph>& graphs, const TimePathResult& r, const StopDirectory& dir,
               int startHour, int startMinute, int srcId, int dstId, double alpha,
               bool extremeCongestion = false) {
    if (extremeCongestion) {
        std::printf("\n[%02d:%02d 출발 | %s -> %s | 극단적 회피(혼잡도*시간)]\n",
                    startHour, startMinute,
                    dir.formatStop(srcId).c_str(), dir.formatStop(dstId).c_str());
    } else {
        std::printf("\n[%02d:%02d 출발 | %s -> %s | alpha=%.2f]\n",
                    startHour, startMinute,
                    dir.formatStop(srcId).c_str(), dir.formatStop(dstId).c_str(), alpha);
    }

    if (!r.found()) {
        std::cout << "  -> 두 정류장 간 이동 가능한 경로가 없습니다.\n";
        return;
    }

    int transfers = 0;
    int prevRouteId = -1;
    double weightedCong = 0.0, validTime = 0.0;

    for (const TimeStep& step : r.steps) {
        const Edge& e = graphs[step.hour].edge(step.edgeIdx);
        if (prevRouteId != -1 && e.routeId != prevRouteId) ++transfers;
        prevRouteId = e.routeId;
        if (e.hasData()) {
            weightedCong += e.congestion * e.travelTime;
            validTime += e.travelTime;
        }
    }
    std::string avgCongStr = "N/A";
    if (validTime > 0.0) {
        char avgBuf[32];
        std::snprintf(avgBuf, sizeof(avgBuf), "%.3f", weightedCong / validTime);
        avgCongStr = avgBuf;
    }
    long long startSec = static_cast<long long>(startHour) * 3600LL + static_cast<long long>(startMinute) * 60LL;
    long long endSec = startSec + static_cast<long long>(std::floor(r.totalTime));

    std::printf("  도착 예정: %s | 총 비용: %.1f | 총 소요시간: %.0f초(%.1f분) | 평균 혼잡도: %s | 환승: %d회 (%zu구간)\n",
                formatClock(endSec).c_str(),
                r.cost, r.totalTime, r.totalTime / 60.0, avgCongStr.c_str(), transfers, r.steps.size());

    prevRouteId = -1;
    for (size_t i = 0; i < r.steps.size(); ++i) {
        const TimeStep& step = r.steps[i];
        const Edge& e = graphs[step.hour].edge(step.edgeIdx);
        bool isTransfer = (prevRouteId != -1 && e.routeId != prevRouteId);

        if (e.hasData()) {
            std::printf("   %2zu. [%02d시|%s~%s] [%s%-8s] %s -> %s | %.0f초 | 혼잡도: %.3f | 재차: %.1f명\n",
                        i + 1,
                        step.hour,
                        formatClock(step.depSec).c_str(),
                        formatClock(step.arrSec).c_str(),
                        isTransfer ? "환승:" : "노선:",
                        e.routeName.c_str(),
                        dir.formatStop(e.srcId).c_str(),
                        dir.formatStop(e.dstId).c_str(),
                        e.travelTime,
                        e.congestion,
                        e.load >= 0.0 ? e.load : 0.0);
        } else {
            std::printf("   %2zu. [%02d시|%s~%s] [%s%-8s] %s -> %s | %.0f초 | 혼잡도: N/A | 재차: N/A\n",
                        i + 1,
                        step.hour,
                        formatClock(step.depSec).c_str(),
                        formatClock(step.arrSec).c_str(),
                        isTransfer ? "환승:" : "노선:",
                        e.routeName.c_str(),
                        dir.formatStop(e.srcId).c_str(),
                        dir.formatStop(e.dstId).c_str(),
                        e.travelTime);
        }
        prevRouteId = e.routeId;
    }
}

// 탐색된 경로를 CSV 파일로 저장 (각 구간이 참조한 시간대 hour 기록, 신규 생성 시 UTF-8 BOM 및 헤더 기록)
bool savePathToCsv(const std::string& csvPath, const ds::Vector<Graph>& graphs,
                   const TimePathResult& r, const StopDirectory& dir, double alpha,
                   bool append = true, bool extremeCongestion = false) {
    if (!r.found() && append) return true;

    std::error_code ec;
    fs::path p = toFsPath(csvPath);
    bool writeHeader = !append || !fs::exists(p, ec) || fs::file_size(p, ec) == 0;
    std::ios::openmode mode = std::ios::binary | (append ? std::ios::app : std::ios::trunc);
    std::ofstream f(p, mode);
    if (!f) return false;

    if (writeHeader) {
        f.write("\xEF\xBB\xBF", 3);
        f << "hour,alpha,step,route_id,route_name,src_id,src_name,dst_id,dst_name,"
             "travel_time,load,congestion,step_cost,is_transfer\n";
    }
    if (!r.found()) {
        f.flush();
        return static_cast<bool>(f);
    }

    int prevRouteId = -1;
    for (size_t i = 0; i < r.steps.size(); ++i) {
        const TimeStep& step = r.steps[i];
        const Edge& e = graphs[step.hour].edge(step.edgeIdx);
        bool isTransfer = (prevRouteId != -1 && e.routeId != prevRouteId);
        auto srcIt = dir.idToName.find(e.srcId);
        auto dstIt = dir.idToName.find(e.dstId);
        const std::string srcName = (srcIt != dir.idToName.end()) ? srcIt->second : "";
        const std::string dstName = (dstIt != dir.idToName.end()) ? dstIt->second : "";

        f << step.hour << ',' << alpha << ',' << (i + 1) << ','
          << e.routeId << ',' << csvEscape(e.routeName) << ','
          << e.srcId << ',' << csvEscape(srcName) << ','
          << e.dstId << ',' << csvEscape(dstName) << ','
          << e.travelTime << ',';
        if (e.load >= 0.0) f << e.load;
        f << ',';
        if (e.hasData()) f << e.congestion;
        const double stepCost = extremeCongestion ? e.extremeCost() : e.cost(alpha);
        f << ',' << stepCost << ',' << (isTransfer ? 1 : 0) << '\n';

        prevRouteId = e.routeId;
    }
    f.flush();
    return static_cast<bool>(f);
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::string folder = resolveDefaultGraphFolder();
    double alpha = 1.0;  // 혼잡도 가중치 기본값
    std::string stopsCsv = "stops.csv";

    ds::Vector<std::string> posArgs;
    bool queryMode = false;
    bool checkPairsMode = false;
    bool includeExtreme = false;
    int queryHour = 0;
    int queryMinute = 0;
    std::string querySrc;
    std::string queryDst;
    std::string queryOutCsv = "path_results.csv";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--extreme") {
            includeExtreme = true;
        } else if (arg == "--query" || arg == "--check-pairs") {
            bool isCheckPairs = (arg == "--check-pairs");
            if (i + 4 >= argc) {
                std::cerr << "[오류] " << arg << " 사용법: " << arg << " <시(0~23)> <분(0~59)> <시작정류장> <도착정류장> [출력CSV]\n";
                return 1;
            }
            if (!tryParseInt(argv[++i], queryHour) || queryHour < 0 || queryHour >= 24) {
                std::cerr << "[오류] 잘못된 시(0~23) 값입니다.\n";
                return 1;
            }
            if (!tryParseInt(argv[++i], queryMinute) || queryMinute < 0 || queryMinute >= 60) {
                std::cerr << "[오류] 잘못된 분(0~59) 값입니다.\n";
                return 1;
            }
            querySrc = argv[++i];
            queryDst = argv[++i];
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                queryOutCsv = argv[++i];
            }
            if (isCheckPairs) checkPairsMode = true;
            else queryMode = true;
        } else {
            posArgs.push_back(arg);
        }
    }

    if (posArgs.size() >= 1) folder = posArgs[0];
    if (posArgs.size() >= 2) {
        const std::string& alphaArg = posArgs[1];
        try {
            size_t parsed = 0;
            alpha = std::stod(alphaArg, &parsed);
            if (parsed != alphaArg.size() || !std::isfinite(alpha) || alpha < 0.0) {
                throw std::invalid_argument("invalid alpha");
            }
        } catch (...) {
            std::cerr << "[오류] 잘못된 alpha 값입니다 (0 이상의 실수 필요): " << alphaArg << '\n';
            return 1;
        }
    }
    if (posArgs.size() >= 3) stopsCsv = posArgs[2];

    StopDirectory dir;
    dir.loadCsv(stopsCsv);

    ds::Vector<Graph> graphs;
    try {
        graphs = loadAllGraphs(folder);
    } catch (const std::exception& ex) {
        std::cerr << "[오류] " << ex.what() << "\n"
                  << "사용법: " << argv[0]
                  << " [그래프폴더=graphs] [alpha=1.0] [정류장CSV=stops.csv]"
                  << " [--query 시 분 시작정류장 도착정류장 [출력CSV]]\n";
        return 1;
    }

    UnifiedStopIndex stopIndex;
    stopIndex.build(graphs);

    if (checkPairsMode) {
        auto splitTokens = [](const std::string& raw) {
            ds::Vector<std::string> out;
            std::stringstream ss(raw);
            std::string item;
            while (std::getline(ss, item, ',')) {
                if (!item.empty()) out.push_back(item);
            }
            return out;
        };

        ds::Vector<std::string> srcTokens = splitTokens(querySrc);
        ds::Vector<std::string> dstTokens = splitTokens(queryDst);
        ds::Vector<int> srcIds;
        ds::Vector<int> dstIds;
        for (const std::string& tok : srcTokens) {
            int id = dir.pickStopId(graphs, stopIndex, queryHour, tok, "시작 정류장", false);
            if (id >= 0 && ds::find(srcIds.begin(), srcIds.end(), id) == srcIds.end()) {
                srcIds.push_back(id);
            }
        }
        for (const std::string& tok : dstTokens) {
            int id = dir.pickStopId(graphs, stopIndex, queryHour, tok, "도착 정류장", true);
            if (id >= 0 && ds::find(dstIds.begin(), dstIds.end(), id) == dstIds.end()) {
                dstIds.push_back(id);
            }
        }

        std::ofstream f(toFsPath(queryOutCsv), std::ios::binary);
        if (!f) {
            std::cerr << "[오류] 결과 CSV 저장 실패: " << queryOutCsv << '\n';
            return 1;
        }
        f << "src_id,dst_id\n";
        for (int sId : srcIds) {
            ds::Vector<int> reached = findReachableTargetsTimeDependent(
                graphs, stopIndex, sId, dstIds, queryHour, queryMinute);
            for (int dId : reached) {
                f << sId << ',' << dId << '\n';
            }
        }
        f.flush();
        if (!f) {
            std::cerr << "[오류] 결과 CSV 저장 실패: " << queryOutCsv << '\n';
            return 1;
        }
        return 0;
    }

    if (queryMode) {
        auto splitTokens = [](const std::string& raw) {
            ds::Vector<std::string> out;
            std::stringstream ss(raw);
            std::string item;
            while (std::getline(ss, item, ',')) {
                if (!item.empty()) out.push_back(item);
            }
            return out;
        };

        ds::Vector<std::string> srcTokens = splitTokens(querySrc);
        ds::Vector<std::string> dstTokens = splitTokens(queryDst);
        ds::Vector<int> srcIds;
        ds::Vector<int> dstIds;
        for (const std::string& tok : srcTokens) {
            int id = dir.pickStopId(graphs, stopIndex, queryHour, tok, "시작 정류장", false);
            if (id >= 0 && ds::find(srcIds.begin(), srcIds.end(), id) == srcIds.end()) {
                srcIds.push_back(id);
            }
        }
        for (const std::string& tok : dstTokens) {
            int id = dir.pickStopId(graphs, stopIndex, queryHour, tok, "도착 정류장", true);
            if (id >= 0 && ds::find(dstIds.begin(), dstIds.end(), id) == dstIds.end()) {
                dstIds.push_back(id);
            }
        }

        if (srcIds.empty()) {
            std::cerr << "[오류] 시작 정류장(" << querySrc << ")이 " << queryHour << "시 그래프에 존재하지 않습니다.\n";
            return 1;
        }
        if (dstIds.empty()) {
            std::cerr << "[오류] 도착 정류장(" << queryDst << ")이 그래프에 존재하지 않습니다.\n";
            return 1;
        }

        // 우선 입력된 (첫 번째) 정류장 쌍을 탐색하고, 연결 경로가 없으면 인접 후보 쌍 순으로 탐색
        ds::Vector<ds::Pair<size_t, size_t>> pairOrder;
        for (size_t i = 0; i < srcIds.size(); ++i) {
            for (size_t j = 0; j < dstIds.size(); ++j) {
                pairOrder.emplace_back(i, j);
            }
        }
        ds::stableSort(pairOrder.begin(), pairOrder.end(),
                       [](const ds::Pair<size_t, size_t>& a, const ds::Pair<size_t, size_t>& b) {
                           return (a.first + a.second) < (b.first + b.second);
                       });

        int chosenSrc = srcIds[0];
        int chosenDst = dstIds[0];
        TimePathResult fastResult;
        for (const auto& p : pairOrder) {
            TimePathResult cand =
                shortestPathTimeDependent(graphs, stopIndex, srcIds[p.first], dstIds[p.second], queryHour, queryMinute, 0.0);
            if (cand.found()) {
                chosenSrc = srcIds[p.first];
                chosenDst = dstIds[p.second];
                fastResult = ds::move(cand);
                break;
            }
        }

        printPath(graphs, fastResult, dir, queryHour, queryMinute, chosenSrc, chosenDst, 0.0);
        if (!savePathToCsv(queryOutCsv, graphs, fastResult, dir, 0.0, false)) {
            std::cerr << "[오류] 결과 CSV 저장 실패: " << queryOutCsv << '\n';
            return 1;
        }
        if (alpha > 0.0) {
            TimePathResult comfyResult =
                shortestPathTimeDependent(graphs, stopIndex, chosenSrc, chosenDst, queryHour, queryMinute, alpha);
            printPath(graphs, comfyResult, dir, queryHour, queryMinute, chosenSrc, chosenDst, alpha);
            if (!savePathToCsv(queryOutCsv, graphs, comfyResult, dir, alpha, true)) {
                std::cerr << "[오류] 결과 CSV 저장 실패: " << queryOutCsv << '\n';
                return 1;
            }
        }
        if (includeExtreme) {
            TimePathResult extremeResult =
                shortestPathTimeDependent(graphs, stopIndex, chosenSrc, chosenDst, queryHour, queryMinute, 0.0, true);
            printPath(graphs, extremeResult, dir, queryHour, queryMinute, chosenSrc, chosenDst, -1.0, true);
            if (!savePathToCsv(queryOutCsv, graphs, extremeResult, dir, -1.0, true, true)) {
                std::cerr << "[오류] 결과 CSV 저장 실패: " << queryOutCsv << '\n';
                return 1;
            }
        }
        return 0;
    }

    std::cout << "00~23시 그래프 24개 로드 완료 (폴더: " << pathToUtf8(folder)
              << ", 통합 정류장: " << stopIndex.denseToStop.size()
              << "개, 현재 설정 alpha = " << alpha << ")\n";

    while (true) {
        std::cout << "\n[입력] 시(0~23) 분(0~59) 시작정류장 도착정류장 (종료: -1): ";
        int hour = 0;
        if (!(std::cin >> hour)) {
            if (std::cin.eof()) break;
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << "시(0~23)는 정수로 입력해 주세요.\n";
            continue;
        }
        if (hour < 0) break;

        int minute = 0;
        if (!(std::cin >> minute)) {
            if (std::cin.eof()) break;
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << "분(0~59)은 정수로 입력해 주세요.\n";
            continue;
        }

        std::string srcToken, dstToken;
        if (!(std::cin >> srcToken >> dstToken)) break;

        if (hour >= 24) {
            std::cout << "시는 0~23 사이로 입력해 주세요.\n";
            continue;
        }
        if (minute < 0 || minute >= 60) {
            std::cout << "분은 0~59 사이로 입력해 주세요.\n";
            continue;
        }

        int srcId = dir.pickStopId(graphs, stopIndex, hour, srcToken, "시작 정류장", false);
        int dstId = dir.pickStopId(graphs, stopIndex, hour, dstToken, "도착 정류장", true);

        if (srcId < 0) {
            std::cout << "시작 정류장(" << srcToken << ")이 " << hour << "시 그래프에 존재하지 않습니다.\n";
            continue;
        }
        if (dstId < 0) {
            std::cout << "도착 정류장(" << dstToken << ")이 그래프에 존재하지 않습니다.\n";
            continue;
        }

        // 1. 순수 최단 시간 경로 (alpha = 0.0, 시간 흐름 반영)
        TimePathResult fastResult =
            shortestPathTimeDependent(graphs, stopIndex, srcId, dstId, hour, minute, 0.0);
        printPath(graphs, fastResult, dir, hour, minute, srcId, dstId, 0.0);
        bool savedFast = savePathToCsv("path_results.csv", graphs, fastResult, dir, 0.0);

        // 2. 혼잡도 반영 경로 (설정한 alpha 적용, 시간 흐름 반영)
        bool savedComfy = true;
        if (alpha > 0.0) {
            TimePathResult comfyResult =
                shortestPathTimeDependent(graphs, stopIndex, srcId, dstId, hour, minute, alpha);
            printPath(graphs, comfyResult, dir, hour, minute, srcId, dstId, alpha);
            savedComfy = savePathToCsv("path_results.csv", graphs, comfyResult, dir, alpha);
        }

        // 3. 극단적 혼잡 회피 경로 (혼잡도 * 이동시간)
        bool savedExtreme = true;
        if (includeExtreme) {
            TimePathResult extremeResult =
                shortestPathTimeDependent(graphs, stopIndex, srcId, dstId, hour, minute, 0.0, true);
            printPath(graphs, extremeResult, dir, hour, minute, srcId, dstId, -1.0, true);
            savedExtreme = savePathToCsv("path_results.csv", graphs, extremeResult, dir, -1.0, true, true);
        }

        if (fastResult.found()) {
            if (savedFast && savedComfy && savedExtreme) {
                std::cout << "  -> 상세 경로가 'path_results.csv'에 저장되었습니다.\n";
            } else {
                std::cerr << "  [경고] 'path_results.csv' 저장 중 오류가 발생했습니다.\n";
            }
        }
    }
    return 0;
}
