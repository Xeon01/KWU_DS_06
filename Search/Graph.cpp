#include "Graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// ───────────────────────── 생성 ─────────────────────────
int Graph::addNode(int stopId) {
    auto it = idToIdx.find(stopId);
    if (it != idToIdx.end()) return it->second;
    int idx = static_cast<int>(nodes.size());
    Node n;
    n.id = stopId;
    nodes.push_back(std::move(n));
    idToIdx[stopId] = idx;
    return idx;
}

int Graph::addEdge(Edge e) {
    e.u = addNode(e.srcId);
    e.v = addNode(e.dstId);
    int idx = static_cast<int>(edges.size());
    nodes[e.u].out.push_back(idx);
    nodes[e.v].in.push_back(idx);
    edges.push_back(std::move(e));
    return idx;
}

// ───────────────────────── 랜덤 액세스 ─────────────────────────
int Graph::indexOf(int stopId) const {
    auto it = idToIdx.find(stopId);
    return it == idToIdx.end() ? -1 : it->second;
}

Node& Graph::operator[](int stopId) {
    int i = indexOf(stopId);
    if (i < 0) throw std::out_of_range("unknown stop id: " + std::to_string(stopId));
    return nodes[i];
}

const Node& Graph::operator[](int stopId) const {
    int i = indexOf(stopId);
    if (i < 0) throw std::out_of_range("unknown stop id: " + std::to_string(stopId));
    return nodes[i];
}

// ───────────────────────── 인접 정보 ─────────────────────────
std::vector<int> Graph::neighbors(int stopId) const {
    std::vector<int> res;
    int idx = indexOf(stopId);
    if (idx < 0) return res;
    std::unordered_set<int> seen;
    for (int ei : nodes[idx].out) {
        int d = edges[ei].dstId;
        if (seen.insert(d).second) res.push_back(d);
    }
    return res;
}

std::vector<int> Graph::edgesBetween(int srcId, int dstId) const {
    std::vector<int> res;
    int idx = indexOf(srcId);
    if (idx < 0) return res;
    for (int ei : nodes[idx].out)
        if (edges[ei].dstId == dstId) res.push_back(ei);
    return res;
}

std::vector<std::string> Graph::routesAt(int stopId) const {
    int idx = indexOf(stopId);
    if (idx < 0) return {};
    std::set<std::string> s;   // 정렬 + 중복 제거
    const Node& n = nodes[idx];
    for (int ei : n.out) s.insert(edges[ei].routeName);
    for (int ei : n.in) s.insert(edges[ei].routeName);
    return {s.begin(), s.end()};
}

// ───────────────────────── 저장 / 불러오기 ─────────────────────────
// 포맷: "BUSG" | version | hour | capLoad | 노드수 | 노드ID들 | 간선수 | 간선들  (리틀엔디언)
namespace {
const char MAGIC[4] = {'B', 'U', 'S', 'G'};
const int32_t VERSION = 2;
constexpr uint32_t MAX_ROUTE_NAME_LEN = 1024 * 1024;
constexpr int32_t MAX_GRAPH_RECORDS = 50000000;

std::filesystem::path toFsPath(const std::string& p) {
#ifdef _WIN32
    if (p.empty()) return std::filesystem::path();
    auto convert = [&](UINT cp, DWORD flags) -> std::wstring {
        int len = MultiByteToWideChar(cp, flags, p.data(), static_cast<int>(p.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring w(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(cp, flags, p.data(), static_cast<int>(p.size()), &w[0], len);
        return w;
    };
    std::wstring w = convert(CP_UTF8, MB_ERR_INVALID_CHARS);
    if (w.empty()) w = convert(CP_ACP, 0);
    if (!w.empty()) return std::filesystem::path(w);
#endif
    return std::filesystem::path(p);
}

template <class T> void wr(std::ofstream& f, const T& x) { f.write(reinterpret_cast<const char*>(&x), sizeof(T)); }
void wrs(std::ofstream& f, const std::string& s) {
    wr<uint32_t>(f, static_cast<uint32_t>(s.size()));
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}
template <class T> T rd(std::ifstream& f) { T x{}; f.read(reinterpret_cast<char*>(&x), sizeof(T)); return x; }
std::string rds(std::ifstream& f, const std::string& path) {
    uint32_t n = rd<uint32_t>(f);
    if (!f) throw std::runtime_error("corrupted file (string len): " + path);
    if (n > MAX_ROUTE_NAME_LEN) throw std::runtime_error("corrupted file (string too long): " + path);
    std::string s(n, '\0');
    if (n > 0) {
        f.read(&s[0], static_cast<std::streamsize>(n));
        if (!f) throw std::runtime_error("corrupted file (string data): " + path);
    }
    return s;
}
}  // namespace

void Graph::writeAdjList(const std::string& path) const {
    std::ofstream f(toFsPath(path));
    if (!f) throw std::runtime_error("cannot write: " + path);
    for (const Node& n : nodes) {
        f << n.id;
        for (int d : neighbors(n.id)) f << ' ' << d;
        f << '\n';
    }
    f.flush();
    if (!f) throw std::runtime_error("write failed: " + path);
}

// ───────────────────────── 탐색 ─────────────────────────
std::vector<int> Graph::reachable(int srcId) const {
    std::vector<char> seen(nodes.size(), 0);
    std::vector<int> res;
    std::queue<int> q;
    int s = indexOf(srcId);
    if (s < 0) return res;
    seen[s] = 1;
    q.push(s);
    while (!q.empty()) {
        int u = q.front(); q.pop();
        res.push_back(nodes[u].id);
        for (int ei : nodes[u].out) {
            int v = edges[ei].v;
            if (!seen[v]) { seen[v] = 1; q.push(v); }
        }
    }
    return res;
}

PathResult Graph::shortestPath(int srcId, int dstId, double alpha, double defaultCongestion) const {
    PathResult r;
    int s = indexOf(srcId), t = indexOf(dstId);
    if (s < 0 || t < 0) return r;

    const double INF = std::numeric_limits<double>::infinity();
    std::vector<double> dist(nodes.size(), INF);
    std::vector<int> prevEdge(nodes.size(), -1);
    using Item = std::pair<double, int>;                       // (거리, 노드 인덱스)
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;  // 최소 힙

    dist[s] = 0;
    pq.push({0.0, s});
    while (!pq.empty()) {
        auto [d, u] = pq.top(); pq.pop();
        if (u == t) break;
        if (d > dist[u]) continue;
        for (int ei : nodes[u].out) {
            const Edge& e = edges[ei];
            if (!std::isfinite(e.travelTime) || e.travelTime < 0.0) continue;
            double stepCost = e.cost(alpha, defaultCongestion);
            if (!std::isfinite(stepCost) || stepCost < 0.0) continue;
            double nd = d + stepCost;
            if (nd < dist[e.v]) {
                dist[e.v] = nd;
                prevEdge[e.v] = ei;
                pq.push({nd, e.v});
            }
        }
    }
    if (dist[t] == INF) return r;

    r.cost = dist[t];
    for (int cur = t; cur != s; cur = edges[prevEdge[cur]].u) r.edges.push_back(prevEdge[cur]);
    std::reverse(r.edges.begin(), r.edges.end());
    for (int ei : r.edges) r.totalTime += edges[ei].travelTime;
    return r;
}

void Graph::save(const std::string& path) const {
    std::ofstream f(toFsPath(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot write: " + path);
    f.write(MAGIC, 4);
    wr(f, VERSION);
    wr<int32_t>(f, hour);
    wr(f, capLoad);
    wr<int32_t>(f, numNodes());
    for (const Node& n : nodes) wr<int32_t>(f, n.id);
    wr<int32_t>(f, numEdges());
    for (const Edge& e : edges) {
        wr<int32_t>(f, e.srcId); wr<int32_t>(f, e.dstId);
        wr<int32_t>(f, e.routeId); wrs(f, e.routeName); wr<int32_t>(f, e.seq);
        wr(f, e.travelTime); wr(f, e.load); wr(f, e.congestion);
    }
    f.flush();
    if (!f) throw std::runtime_error("write failed: " + path);
}

Graph Graph::load(const std::string& path) {
    std::ifstream f(toFsPath(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open: " + path);
    char m[4] = {};
    f.read(m, 4);
    if (!f || std::string(m, 4) != "BUSG") throw std::runtime_error("not a graph file: " + path);
    if (rd<int32_t>(f) != VERSION || !f) throw std::runtime_error("version mismatch: " + path);

    Graph g;
    g.hour = rd<int32_t>(f);
    g.capLoad = rd<double>(f);
    int32_t nn = rd<int32_t>(f);
    if (!f || nn < 0 || nn > MAX_GRAPH_RECORDS) throw std::runtime_error("corrupted node count: " + path);
    g.nodes.reserve(static_cast<size_t>(nn));
    g.idToIdx.reserve(static_cast<size_t>(nn) * 2);
    for (int32_t i = 0; i < nn; ++i) {
        int32_t nid = rd<int32_t>(f);
        if (!f) throw std::runtime_error("corrupted node data: " + path);
        g.addNode(nid);
    }
    int32_t ne = rd<int32_t>(f);
    if (!f || ne < 0 || ne > MAX_GRAPH_RECORDS) throw std::runtime_error("corrupted edge count: " + path);
    g.edges.reserve(static_cast<size_t>(ne));
    for (int32_t i = 0; i < ne; ++i) {
        Edge e;
        e.srcId = rd<int32_t>(f); e.dstId = rd<int32_t>(f);
        e.routeId = rd<int32_t>(f); e.routeName = rds(f, path); e.seq = rd<int32_t>(f);
        e.travelTime = rd<double>(f); e.load = rd<double>(f); e.congestion = rd<double>(f);
        if (!f) throw std::runtime_error("corrupted edge data: " + path);
        g.addEdge(std::move(e));
    }
    return g;
}

std::string Graph::summary() const {
    double sum = 0;
    int cnt = 0;
    for (const Edge& e : edges) if (e.hasData()) { sum += e.congestion; ++cnt; }
    char buf[160];
    std::snprintf(buf, sizeof buf, "Graph(hour=%02d, nodes=%d, edges=%d, avg_congestion=%.3f)",
                  hour, numNodes(), numEdges(), cnt ? sum / cnt : 0.0);
    return buf;
}

Graph loadGraph(int hour, const std::string& folder) {
    char name[32];
    std::snprintf(name, sizeof name, "graph_%02d.bin", hour);
    if (folder.empty()) return Graph::load(name);
    char back = folder.back();
    std::string full = (back == '/' || back == '\\') ? (folder + name) : (folder + "/" + name);
    return Graph::load(full);
}
