#include <iostream>
#include <iomanip>
#include <filesystem>
#include "Graph.hpp"
#include "GraphLoader.hpp"
#include "MultiLayerDijkstra.hpp"

using namespace std;

int main() {
    try {
        // 실행 위치(예: build/ 또는 x64/Debug/)에 따라 graphs 폴더 경로 자동 탐색
        string graphDir = "graphs";
        if (!filesystem::exists(graphDir) && filesystem::exists("../graphs")) {
            graphDir = "../graphs";
        }

        // 1. graphs/graph_00.bin ~ graph_23.bin 로드
        cout << "24시간 그래프 데이터 로딩 중 (" << graphDir << ")...\n";
        vector<Graph> hourlyGraphs = loadAllHourlyGraphs(graphDir);
        cout << "로딩 완료!\n\n";

        // 2. 멀티레이어 다익스트라 초기화
        MultiLayerDijkstra pathFinder(hourlyGraphs);

        // 3. 경로 탐색 테스트 (예: 07시 50분 출발 -> 이동 중 08시 레이어로 자동 전환)
        int startStopId = 100000001;
        int endStopId   = 102000072;
        double departSec = 7 * 3600 + 50 * 60; // 07:50:00
        double alpha = 1.5;                    // 혼잡도 가중치 계수 (@)
        double transferPenalty = 300.0;        // 환승 페널티 (300초 = 5분)

        PathResult res = pathFinder.findPath(
            startStopId, endStopId, departSec, alpha, transferPenalty
        );

        if (!res.found) {
            cout << "경로를 찾을 수 없습니다. 정류장 ID를 확인하세요.\n";
            return 0;
        }

        cout << "=== 쾌적 경로 탐색 결과 ===\n";
        cout << "총 체감 비용   : " << fixed << setprecision(1) << res.totalCost << "\n";
        cout << "실제 소요 시간 : " << res.totalPhysicalTime / 60.0 << "분\n";
        cout << "총 환승 횟수   : " << res.transferCount << "회\n\n";

        for (const auto& step : res.steps) {
            int arrH = static_cast<int>(step.arrivalTime / 3600) % 24;
            int arrM = static_cast<int>(step.arrivalTime / 60) % 60;
            int arrS = static_cast<int>(step.arrivalTime) % 60;

            cout << "[" << setfill('0') << setw(2) << step.hourLayer << "시] "
                 << step.srcStopId << " -> " << step.dstStopId
                 << " | 노선: " << step.routeName
                 << " | 이동: " << fixed << setprecision(0) << step.travelTime << "초"
                 << " | 혼잡도: " << fixed << setprecision(2) << step.congestion
                 << " | 도착: " << setw(2) << arrH << ":" << setw(2) << arrM << ":" << setw(2) << arrS << "\n";
        }
    } catch (const exception& e) {
        cerr << "[오류] " << e.what() << "\n";
        return 1;
    }

    return 0;
}