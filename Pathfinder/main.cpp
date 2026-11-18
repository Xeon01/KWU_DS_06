#include <iostream>
#include <iomanip>
#include <filesystem>
#include <cstdlib>
#include "Graph.hpp"
#include "MultiLayerDijkstra.hpp"

using namespace std;

void printResult(const string& title, const MultiLayerPathResult& res) {
    cout << "=== " << title << " ===\n";
    if (!res.found) {
        cout << "경로를 찾을 수 없습니다.\n\n";
        return;
    }

    cout << "총 체감 비용   : " << fixed << setprecision(1) << res.totalCost << "\n";
    cout << "실제 소요 시간 : " << res.totalPhysicalTime / 60.0 << "분 ("
         << setprecision(0) << res.totalPhysicalTime << "초)\n";
    cout << "총 환승 횟수   : " << res.transferCount << "회\n";
    cout << "경유 정류장 수 : " << res.steps.size() << "구간\n\n";

    for (const auto& step : res.steps) {
        int arrH = static_cast<int>(step.arrivalTime / 3600) % 24;
        int arrM = static_cast<int>(step.arrivalTime / 60) % 60;
        int arrS = static_cast<int>(step.arrivalTime) % 60;

        cout << "[" << setfill('0') << setw(2) << step.hourLayer << "시] "
             << step.srcStopId << " -> " << step.dstStopId
             << " | 노선: " << step.routeName
             << " | 이동: " << fixed << setprecision(0) << step.travelTime << "초"
             << " | 혼잡도: " << fixed << setprecision(2) << step.congestion;
        if (step.hasCongestionData) {
            cout << " (재차 " << fixed << setprecision(1) << step.load << "명)";
        } else {
            cout << " (데이터없음)";
        }
        cout << " | 도착: " << setfill('0') << setw(2) << arrH << ":"
             << setw(2) << arrM << ":" << setw(2) << arrS << "\n";
    }
    cout << "\n";
}

int main(int argc, char* argv[]) {
    try {
        string graphDir = "graphs";
        if (!filesystem::exists(graphDir) && filesystem::exists("../graphs")) {
            graphDir = "../graphs";
        }

        // 1. Graph.cpp의 loadGraph()로 24개 시간대 바이너리 그래프 로드
        cout << "24시간 그래프 데이터 로딩 중 (" << graphDir << ")...\n";
        vector<Graph> hourlyGraphs;
        hourlyGraphs.reserve(24);
        for (int h = 0; h < 24; ++h) {
            hourlyGraphs.push_back(loadGraph(h, graphDir));
        }
        cout << "로딩 완료! (" << hourlyGraphs[8].summary() << ")\n\n";

        // 2. 멀티레이어 다익스트라 초기화
        MultiLayerDijkstra pathFinder(hourlyGraphs);

        // 3. 기본 테스트 파라미터 (인자 전달 시 해당 값 사용)
        // 사용법: ./pathfinder [출발ID] [도착ID] [출발시] [출발분] [alpha] [환승페널티(초)]
        int startStopId = 100000001;
        int endStopId   = 102000072;
        int depHour = 7;
        int depMin  = 50;
        double alpha = 1.5;
        double transferPenalty = 300.0;

        if (argc >= 3) {
            startStopId = stoi(argv[1]);
            endStopId   = stoi(argv[2]);
        }
        if (argc >= 5) {
            depHour = stoi(argv[3]);
            depMin  = stoi(argv[4]);
        }
        if (argc >= 6) alpha = stod(argv[5]);
        if (argc >= 7) transferPenalty = stod(argv[6]);

        // 기본 예시 ID가 현재 그래프에 없으면 그래프 내 유효 정류장 쌍 자동 선택
        if (argc < 3 && (!hourlyGraphs[depHour].hasNode(startStopId) || !hourlyGraphs[depHour].hasNode(endStopId))) {
            for (const Node& n : hourlyGraphs[depHour].nodes) {
                vector<int> reach = hourlyGraphs[depHour].reachable(n.id);
                if (reach.size() > 15) {
                    startStopId = reach.front();
                    endStopId   = reach[reach.size() / 2];
                    break;
                }
            }
        }

        double departSec = depHour * 3600.0 + depMin * 60.0;
        cout << "탐색 조건: " << startStopId << " -> " << endStopId
             << " | 출발: " << setfill('0') << setw(2) << depHour << ":" << setw(2) << depMin
             << " | alpha=" << alpha << " | 환승페널티=" << transferPenalty << "초\n\n";

        // 비교 1: 혼잡도 미반영 (alpha = 0.0, 기존 최단 시간 기준)
        MultiLayerPathResult shortestRes = pathFinder.findPath(
            startStopId, endStopId, departSec, 0.0, transferPenalty
        );
        printResult("1) 최단 시간 경로 (alpha = 0.0)", shortestRes);

        // 비교 2: 시간대별 혼잡도 가중치 반영 쾌적 경로
        MultiLayerPathResult comfortRes = pathFinder.findPath(
            startStopId, endStopId, departSec, alpha, transferPenalty
        );
        printResult("2) 혼잡도 가중치 쾌적 경로 (alpha = " + to_string(alpha).substr(0, 3) + ")", comfortRes);

    } catch (const exception& e) {
        cerr << "[오류] " << e.what() << "\n";
        return 1;
    }

    return 0;
}