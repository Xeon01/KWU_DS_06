// 버스 정류장 그래프 자료구조
//
//   Graph ─ nodes     : std::vector<Node>              인덱스 → 노드      (배열, O(1))
//           idToIdx   : std::unordered_map<int,int>    정류장ID → 인덱스  (해시맵, 평균 O(1))
//           edges     : std::vector<Edge>              모든 간선
//   Node  ─ out / in  : std::vector<int>               인접 리스트 / 역인접 리스트 (edges 의 인덱스)
//
// 간선은 노선 단위 (같은 정류장 쌍을 여러 노선이 지나면 간선도 여러 개) → 방향 있는 멀티그래프.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

struct Edge {
    int u = -1, v = -1;          // 출발/도착 노드 인덱스 (nodes[u], nodes[v])
    int srcId = 0, dstId = 0;    // 출발/도착 정류장 ID
    int routeId = 0;             // 노선 ID
    std::string routeName;       // 노선명 (예: "470")
    int seq = 0;                 // 출발 정류장 순서
    double travelTime = 0;       // 기본 이동시간 (초)
    double load = -1;            // 이 시간대 재차인원 (하루 평균, 명). 데이터 없으면 -1
    double congestion = -1;      // 혼잡도 0~1 (1 = 가장 혼잡). 데이터 없으면 -1

    bool hasData() const { return congestion >= 0; }
    // 경로 탐색 가중치 = 이동시간 × (1 + alpha × 혼잡도)
    double cost(double alpha, double defaultCongestion = 0.0) const {
        double c = hasData() ? congestion : defaultCongestion;
        return travelTime * (1.0 + alpha * c);
    }
};

struct Node {
    int id = 0;                  // 정류장 ID
    std::vector<int> out;        // 나가는 간선 인덱스 (인접 리스트)
    std::vector<int> in;         // 들어오는 간선 인덱스 (역인접 리스트)
};

struct PathResult {
    double cost = -1;            // 총 비용 (경로 없으면 -1)
    double totalTime = 0;        // 총 이동시간 (초)
    std::vector<int> edges;      // 지나가는 간선 인덱스 (순서대로)
    bool found() const { return cost >= 0; }
};

class Graph {
public:
    int hour = -1;               // 0~23 (몇 시 그래프인지)
    double capLoad = 0;          // 혼잡도 1.0 에 해당하는 재차인원

    // ---------- 생성 ----------
    int addNode(int stopId);
    int addEdge(Edge e);         // e.srcId, e.dstId 로 노드를 찾아(없으면 생성) 연결

    // ---------- 랜덤 액세스 ----------
    bool hasNode(int stopId) const { return idToIdx.count(stopId) > 0; }
    int indexOf(int stopId) const;                 // 정류장ID → 인덱스, 없으면 -1
    Node& operator[](int stopId);                  // g[정류장ID] → Node, 평균 O(1)
    const Node& operator[](int stopId) const;
    Node& nodeAt(int i) { return nodes[i]; }       // g.nodeAt(i) → Node, O(1)
    const Node& nodeAt(int i) const { return nodes[i]; }
    const Edge& edge(int i) const { return edges[i]; }

    int numNodes() const { return static_cast<int>(nodes.size()); }
    int numEdges() const { return static_cast<int>(edges.size()); }

    // ---------- 인접 정보 (없는 정류장 ID는 빈 벡터 반환) ----------
    std::vector<int> neighbors(int stopId) const;              // 바로 갈 수 있는 정류장ID (중복 제거)
    std::vector<int> edgesBetween(int srcId, int dstId) const; // src→dst 직행 간선 인덱스들
    std::vector<std::string> routesAt(int stopId) const;       // 이 정류장을 지나는 노선명
    void writeAdjList(const std::string& path) const;          // "출발ID 이웃ID 이웃ID ..." 텍스트

    // ---------- 탐색 ----------
    std::vector<int> reachable(int srcId) const;               // BFS (std::queue)
    PathResult shortestPath(int srcId, int dstId, double alpha = 0.0,
                            double defaultCongestion = 0.0) const;  // Dijkstra (std::priority_queue)

    // ---------- 저장 / 불러오기 (바이너리) ----------
    void save(const std::string& path) const;
    static Graph load(const std::string& path);

    std::string summary() const;

    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::unordered_map<int, int> idToIdx;
};

// loadGraph(8, "Search/graphs") → Search/graphs/graph_08.bin
Graph loadGraph(int hour, const std::string& folder = "Search/graphs");
