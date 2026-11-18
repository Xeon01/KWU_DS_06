#pragma once
#include <vector>
#include <queue>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <limits>
#include <iostream>
#include <iomanip>
#include "Graph.hpp" // 기존 Edge, Node, Graph 정의 포함

using namespace std;

// 역추적된 개별 이동 구간 정보 (Step 4 지도 시각화 연동용)
struct PathStep {
    int hourLayer;        // 해당 구간 탑승 시간대 (0~23)
    int srcStopId;        // 출발 정류장 ID
    int dstStopId;        // 도착 정류장 ID
    string routeName;     // 노선명 (예: "470")
    double travelTime;    // 구간 물리 이동시간 (초)
    double congestion;    // 해당 시간대 혼잡도 (0~1)
    double arrivalTime;   // 도착 시각 (자정 기준 누적 초)
};

// 최종 탐색 결과 구조체
struct PathResult {
    bool found = false;
    double totalCost = 0.0;          // 혼잡도 및 환승 페널티 반영 총 체감 비용
    double totalPhysicalTime = 0.0;  // 실제 총 소요 시간 (초)
    int transferCount = 0;           // 총 환승 횟수
    vector<PathStep> steps;          // 출발지 -> 목적지 순서의 상세 경로
};

class MultiLayerDijkstra {
private:
    const vector<Graph>& graphs;                 // 24시간 그래프 (크기 24)
    vector<int> idxToGlobalId;                   // 통합 인덱스 -> 정류장 ID
    unordered_map<int, int> globalIdToIdx;       // 정류장 ID -> 통합 인덱스
    vector<vector<int>> layerNodeIdx;            // [hour][globalIdx] -> 해당 시간대 Graph 내부 인덱스

    struct State {
        double cost;          // 누적 체감 비용 (Min-Heap 기준)
        double currentTime;   // 현재 시각 (초)
        int u;                // 통합 정류장 인덱스
        string lastRoute;     // 직전 탑승 노선명 (환승 판별용)

        bool operator>(const State& other) const {
            return cost > other.cost;
        }
    };

    struct ParentInfo {
        int prevHour = -1;
        int prevNode = -1;
        int edgeIdx = -1;     // graphs[prevHour].edges 내 인덱스
        double arrivalTime = 0.0;
    };

public:
    explicit MultiLayerDijkstra(const vector<Graph>& hourlyGraphs) : graphs(hourlyGraphs) {
        // 1. 24개 그래프의 모든 정류장 ID를 통합 인덱스로 매핑
        for (int h = 0; h < static_cast<int>(graphs.size()); ++h) {
            for (const auto& node : graphs[h].nodes) {
                if (globalIdToIdx.find(node.id) == globalIdToIdx.end()) {
                    int newIdx = static_cast<int>(idxToGlobalId.size());
                    globalIdToIdx[node.id] = newIdx;
                    idxToGlobalId.push_back(node.id);
                }
            }
        }

        // 2. [시간대][통합인덱스] -> 각 그래프 로컬 인덱스 캐시 테이블 구축 (O(1) 조회용)
        int V = static_cast<int>(idxToGlobalId.size());
        layerNodeIdx.assign(24, vector<int>(V, -1));
        for (int h = 0; h < 24 && h < static_cast<int>(graphs.size()); ++h) {
            for (const auto& kv : graphs[h].idToIdx) {
                int stopId = kv.first;
                int localIdx = kv.second;
                int gIdx = globalIdToIdx[stopId];
                layerNodeIdx[h][gIdx] = localIdx;
            }
        }
    }

    /**
     * @param startStopId     출발 정류장 ID
     * @param endStopId       도착 정류장 ID
     * @param startTimeSec    출발 시각 (초 단위, 예: 07:50 -> 7*3600 + 50*60 = 28200)
     * @param alpha           혼잡도 가중치 계수 (@) : 비용 = 이동시간 * (1 + alpha * 혼잡도)
     * @param transferPenalty 노선 변경(환승) 시 추가되는 체감 시간 페널티 (초 단위, 예: 300.0)
     */
    PathResult findPath(int startStopId, int endStopId, double startTimeSec,
                        double alpha = 1.0, double transferPenalty = 300.0) const {
        PathResult result;
        if (graphs.size() < 24) return result;
        if (globalIdToIdx.find(startStopId) == globalIdToIdx.end() ||
            globalIdToIdx.find(endStopId) == globalIdToIdx.end()) {
            return result;
        }

        int srcU = globalIdToIdx.at(startStopId);
        int dstU = globalIdToIdx.at(endStopId);
        int V = static_cast<int>(idxToGlobalId.size());
        const double INF = numeric_limits<double>::infinity();

        // 24개 레이어 x V개 정류장 비용 및 역추적 배열
        vector<vector<double>> minCost(24, vector<double>(V, INF));
        vector<vector<ParentInfo>> parent(24, vector<ParentInfo>(V));

        priority_queue<State, vector<State>, greater<State>> pq;

        // 시작 시각 정규화 (0 ~ 86399초)
        double normalizedStart = fmod(fmod(startTimeSec, 86400.0) + 86400.0, 86400.0);
        int startHour = static_cast<int>(normalizedStart / 3600.0) % 24;

        minCost[startHour][srcU] = 0.0;
        pq.push({0.0, normalizedStart, srcU, ""});

        int bestFinalHour = -1;

        while (!pq.empty()) {
            State curr = pq.top();
            pq.pop();

            int currHour = static_cast<int>(curr.currentTime / 3600.0) % 24;

            // 이미 더 적은 비용으로 해당 시간대 레이어의 정류장에 도달한 경우 스킵
            if (curr.cost > minCost[currHour][curr.u] + 1e-9) continue;

            // 목적지 최초 pop 시 전역 최소 비용 보장 (모든 간선 비용 >= 0)
            if (curr.u == dstU) {
                bestFinalHour = currHour;
                result.found = true;
                result.totalCost = curr.cost;
                result.totalPhysicalTime = curr.currentTime - normalizedStart;
                break;
            }

            // 현재 시각에 해당하는 시간대 레이어(graphs[currHour])에서 진출 간선 조회
            int localU = layerNodeIdx[currHour][curr.u];
            if (localU == -1) continue;

            const Graph& currentGraph = graphs[currHour];
            for (int edgeIdx : currentGraph.nodes[localU].out) {
                const Edge& e = currentGraph.edges[edgeIdx];

                auto itDst = globalIdToIdx.find(e.dstId);
                if (itDst == globalIdToIdx.end()) continue;
                int nextU = itDst->second;

                // 1. 물리 시간 갱신 및 다음 시간대 레이어 계산
                double nextTime = curr.currentTime + e.travelTime;
                int nextHour = static_cast<int>(nextTime / 3600.0) % 24;

                // 2. 환승 페널티 계산 (첫 탑승이 아니고 노선명이 달라질 때 부여)
                double penalty = (!curr.lastRoute.empty() && curr.lastRoute != e.routeName)
                                 ? transferPenalty : 0.0;

                // 3. 복합 간선 비용 = 이동시간 * (1 + @ * 혼잡도) + 환승 페널티
                double edgeCost = e.travelTime * (1.0 + alpha * e.congestion) + penalty;
                double nextCost = curr.cost + edgeCost;

                // 4. 다음 레이어(nextHour)의 nextU 상태 완화(Relaxation)
                if (nextCost < minCost[nextHour][nextU]) {
                    minCost[nextHour][nextU] = nextCost;
                    parent[nextHour][nextU] = {currHour, curr.u, edgeIdx, nextTime};
                    pq.push({nextCost, nextTime, nextU, e.routeName});
                }
            }
        }

        if (!result.found) return result;

        // 5. 시간대 레이어를 거슬러 올라가며 경로 역추적 (Backtracking)
        int traceHour = bestFinalHour;
        int traceNode = dstU;

        while (!(traceHour == startHour && traceNode == srcU)) {
            const ParentInfo& p = parent[traceHour][traceNode];
            if (p.prevNode == -1) break;

            const Edge& usedEdge = graphs[p.prevHour].edges[p.edgeIdx];
            result.steps.push_back({
                p.prevHour,
                usedEdge.srcId,
                usedEdge.dstId,
                usedEdge.routeName,
                usedEdge.travelTime,
                usedEdge.congestion,
                p.arrivalTime
            });

            traceHour = p.prevHour;
            traceNode = p.prevNode;
        }

        reverse(result.steps.begin(), result.steps.end());

        // 환승 횟수 집계
        for (size_t i = 1; i < result.steps.size(); ++i) {
            if (result.steps[i].routeName != result.steps[i - 1].routeName) {
                result.transferCount++;
            }
        }

        return result;
    }
};