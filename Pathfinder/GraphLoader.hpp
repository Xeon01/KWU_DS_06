#pragma once
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include "Graph.hpp"

using namespace std;

// 단일 시간대 바이너리 파일(graph_XX.bin) 로드
inline bool loadGraphFromBin(const string& filepath, Graph& g) {
    ifstream ifs(filepath, ios::binary);
    if (!ifs.is_open()) {
        return false;
    }

    g.nodes.clear();
    g.edges.clear();
    g.idToIdx.clear();

    // 1. 노드 리스트 읽기 및 idToIdx 해시 테이블 복원
    size_t nodeCount = 0;
    ifs.read(reinterpret_cast<char*>(&nodeCount), sizeof(nodeCount));
    g.nodes.resize(nodeCount);

    for (size_t i = 0; i < nodeCount; ++i) {
        ifs.read(reinterpret_cast<char*>(&g.nodes[i].id), sizeof(int));
        g.idToIdx[g.nodes[i].id] = static_cast<int>(i);

        size_t outSize = 0;
        ifs.read(reinterpret_cast<char*>(&outSize), sizeof(outSize));
        g.nodes[i].out.resize(outSize);
        if (outSize > 0) {
            ifs.read(reinterpret_cast<char*>(g.nodes[i].out.data()), outSize * sizeof(int));
        }

        size_t inSize = 0;
        ifs.read(reinterpret_cast<char*>(&inSize), sizeof(inSize));
        g.nodes[i].in.resize(inSize);
        if (inSize > 0) {
            ifs.read(reinterpret_cast<char*>(g.nodes[i].in.data()), inSize * sizeof(int));
        }
    }

    // 2. 간선 리스트 읽기
    size_t edgeCount = 0;
    ifs.read(reinterpret_cast<char*>(&edgeCount), sizeof(edgeCount));
    g.edges.resize(edgeCount);

    for (size_t i = 0; i < edgeCount; ++i) {
        Edge& e = g.edges[i];
        ifs.read(reinterpret_cast<char*>(&e.u), sizeof(int));
        ifs.read(reinterpret_cast<char*>(&e.v), sizeof(int));
        ifs.read(reinterpret_cast<char*>(&e.srcId), sizeof(int));
        ifs.read(reinterpret_cast<char*>(&e.dstId), sizeof(int));

        size_t nameLen = 0;
        ifs.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        e.routeName.resize(nameLen);
        if (nameLen > 0) {
            ifs.read(&e.routeName[0], nameLen);
        }

        ifs.read(reinterpret_cast<char*>(&e.travelTime), sizeof(double));
        ifs.read(reinterpret_cast<char*>(&e.congestion), sizeof(double));
    }

    return ifs.good() || ifs.eof();
}

// graphs/graph_00.bin ~ graphs/graph_23.bin 일괄 로드
inline vector<Graph> loadAllHourlyGraphs(const string& dirPath = "graphs") {
    vector<Graph> hourlyGraphs(24);

    for (int h = 0; h < 24; ++h) {
        ostringstream oss;
        oss << dirPath << "/graph_" << setfill('0') << setw(2) << h << ".bin";
        string filepath = oss.str();

        if (!loadGraphFromBin(filepath, hourlyGraphs[h])) {
            throw runtime_error("그래프 파일을 열 수 없거나 손상되었습니다: " + filepath);
        }
    }

    return hourlyGraphs;
}