#pragma once
#include <vector>
#include <queue>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <limits>
#include <cmath>
#include "Graph.hpp"

using namespace std;

// 역추적된 개별 이동 구간 정보
struct PathStep {
    int hourLayer;           // 해당 구간 탑승 시간대 (0~23)
    int srcStopId;           // 출발 정류장 ID
    int dstStopId;           // 도착 정류장 ID
    int routeId;             // 노선 ID
    string routeName;        // 노선명 (예: "470")
    double travelTime;       // 구간 물리 이동시간 (초)
    double load;             // 시간대 평균 재차인원 (명, 데이터 없으면 -1)
    double congestion;       // 적용된 혼잡도 (0~1)
    bool hasCongestionData;  // 실제 혼잡도 데이터 존재 여부
    double arrivalTime;      // 도착 시각 (자정 기준 누적 초)
};

// 멀티레이어 경로 탐색 결과
struct MultiLayerPathResult {
    bool found = false;
    double totalCost = 0.0;          // 혼잡도 및 환승 페널티 반영 총 체감 비용
    double totalPhysicalTime = 0.0;  // 실제 총 소요 시간 (초)
    int transferCount = 0;           // 총 환승 횟수
    vector<PathStep> steps;          // 출발지 -> 목적지 순서의 상세 경로
};

class MultiLayerDijkstra {
private:
    const vector<Graph>& graphs;
    vector<int> idxToGlobalId;
    unordered_map<int, int> globalIdToIdx;
    vector<vector<int>> layerNodeIdx;

    struct State {
        double cost;
        double currentTime;
        int u;
        int lastRouteId;
        string lastRouteName;

        bool operator>(const State& other) const {
            return cost > other.cost;
        }
    };

    struct ParentInfo {
        int prevHour = -1;
        int prevNode = -1;
        int edgeIdx = -1;
        double arrivalTime = 0.0;
    };

    static bool isTransfer(int prevRouteId, const string& prevRouteName, const Edge& nextEdge) {
        if (prevRouteId == -1 && prevRouteName.empty()) return false;
        if (prevRouteId > 0 && nextEdge.routeId > 0) {
            return prevRouteId != nextEdge.routeId;
        }
        return prevRouteName != nextEdge.routeName;
    }

public:
    explicit MultiLayerDijkstra(const vector<Graph>& hourlyGraphs) : graphs(hourlyGraphs) {
        for (int h = 0; h < static_cast<int>(graphs.size()); ++h) {
            for (const auto& node : graphs[h].nodes) {
                if (globalIdToIdx.find(node.id) == globalIdToIdx.end()) {
                    int newIdx = static_cast<int>(idxToGlobalId.size());
                    globalIdToIdx[node.id] = newIdx;
                    idxToGlobalId.push_back(node.id);
                }
            }
        }

        int V = static_cast<int>(idxToGlobalId.size());
        layerNodeIdx.assign(24, vector<int>(V, -1));
        for (int h = 0; h < 24 && h < static_cast<int>(graphs.size()); ++h) {
            for (const auto& kv : graphs[h].idToIdx) {
                layerNodeIdx[h][globalIdToIdx[kv.first]] = kv.second;
            }
        }
    }

    MultiLayerPathResult findPath(int startStopId, int endStopId, double startTimeSec,
                                  double alpha = 1.0, double transferPenalty = 300.0,
                                  double defaultCongestion = 0.0) const {
        MultiLayerPathResult result;
        if (graphs.size() < 24) return result;
        if (globalIdToIdx.find(startStopId) == globalIdToIdx.end() ||
            globalIdToIdx.find(endStopId) == globalIdToIdx.end()) {
            return result;
        }

        int srcU = globalIdToIdx.at(startStopId);
        int dstU = globalIdToIdx.at(endStopId);
        int V = static_cast<int>(idxToGlobalId.size());
        const double INF = numeric_limits<double>::infinity();

        vector<vector<double>> minCost(24, vector<double>(V, INF));
        vector<vector<ParentInfo>> parent(24, vector<ParentInfo>(V));

        priority_queue<State, vector<State>, greater<State>> pq;

        double normalizedStart = std::fmod(std::fmod(startTimeSec, 86400.0) + 86400.0, 86400.0);
        int startHour = static_cast<int>(normalizedStart / 3600.0) % 24;

        minCost[startHour][srcU] = 0.0;
        pq.push({0.0, normalizedStart, srcU, -1, ""});

        int bestFinalHour = -1;

        while (!pq.empty()) {
            State curr = pq.top();
            pq.pop();

            int currHour = static_cast<int>(curr.currentTime / 3600.0) % 24;

            if (curr.cost > minCost[currHour][curr.u] + 1e-9) continue;

            if (curr.u == dstU) {
                bestFinalHour = currHour;
                result.found = true;
                result.totalCost = curr.cost;
                result.totalPhysicalTime = curr.currentTime - normalizedStart;
                break;
            }

            int localU = layerNodeIdx[currHour][curr.u];
            if (localU == -1) continue;

            const Graph& currentGraph = graphs[currHour];
            for (int edgeIdx : currentGraph.nodes[localU].out) {
                const Edge& e = currentGraph.edges[edgeIdx];

                auto itDst = globalIdToIdx.find(e.dstId);
                if (itDst == globalIdToIdx.end()) continue;
                int nextU = itDst->second;

                double nextTime = curr.currentTime + e.travelTime;
                int nextHour = static_cast<int>(nextTime / 3600.0) % 24;

                double penalty = isTransfer(curr.lastRouteId, curr.lastRouteName, e)
                                 ? transferPenalty : 0.0;

                double edgeCost = e.cost(alpha, defaultCongestion) + penalty;
                double nextCost = curr.cost + edgeCost;

                if (nextCost < minCost[nextHour][nextU]) {
                    minCost[nextHour][nextU] = nextCost;
                    parent[nextHour][nextU] = {currHour, curr.u, edgeIdx, nextTime};
                    pq.push({nextCost, nextTime, nextU, e.routeId, e.routeName});
                }
            }
        }

        if (!result.found) return result;

        int traceHour = bestFinalHour;
        int traceNode = dstU;

        while (!(traceHour == startHour && traceNode == srcU)) {
            const ParentInfo& p = parent[traceHour][traceNode];
            if (p.prevNode == -1) break;

            const Edge& usedEdge = graphs[p.prevHour].edges[p.edgeIdx];
            double appliedCongestion = usedEdge.hasData() ? usedEdge.congestion : defaultCongestion;

            result.steps.push_back({
                p.prevHour,
                usedEdge.srcId,
                usedEdge.dstId,
                usedEdge.routeId,
                usedEdge.routeName,
                usedEdge.travelTime,
                usedEdge.load,
                appliedCongestion,
                usedEdge.hasData(),
                p.arrivalTime
            });

            traceHour = p.prevHour;
            traceNode = p.prevNode;
        }

        reverse(result.steps.begin(), result.steps.end());

        for (size_t i = 1; i < result.steps.size(); ++i) {
            bool changed = (result.steps[i].routeId > 0 && result.steps[i - 1].routeId > 0)
                           ? (result.steps[i].routeId != result.steps[i - 1].routeId)
                           : (result.steps[i].routeName != result.steps[i - 1].routeName);
            if (changed) result.transferCount++;
        }

        return result;
    }
};