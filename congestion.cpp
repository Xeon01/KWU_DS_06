#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CsvReader {
public:
    explicit CsvReader(const std::string& path) : in_(path, std::ios::binary) {
        if (!in_) throw std::runtime_error("cannot open: " + path);
        std::vector<std::string> header;
        if (!next(header)) throw std::runtime_error("empty file: " + path);
        if (!header.empty() && header[0].rfind("\xEF\xBB\xBF", 0) == 0) header[0] = header[0].substr(3);
        for (size_t i = 0; i < header.size(); ++i) col_[header[i]] = static_cast<int>(i);
        header_ = header;
    }

    bool next(std::vector<std::string>& row) {
        row.clear();
        std::string line;
        if (!std::getline(in_, line)) return false;
        std::string field;
        bool quoted = false;
        for (size_t i = 0;; ++i) {
            if (i == line.size()) {
                if (quoted) {
                    std::string more;
                    if (!std::getline(in_, more)) break;
                    field += '\n';
                    line += '\n' + more;
                    continue;
                }
                break;
            }
            char c = line[i];
            if (quoted) {
                if (c == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') { field += '"'; ++i; }
                    else quoted = false;
                } else field += c;
            } else if (c == '"') quoted = true;
            else if (c == ',') { row.push_back(field); field.clear(); }
            else if (c != '\r') field += c;
        }
        row.push_back(field);
        return true;
    }

    int col(const std::string& name) const {
        auto it = col_.find(name);
        if (it == col_.end()) throw std::runtime_error("column not found: " + name);
        return it->second;
    }
    bool hasCol(const std::string& name) const { return col_.count(name) > 0; }
    const std::vector<std::string>& header() const { return header_; }

private:
    std::ifstream in_;
    std::unordered_map<std::string, int> col_;
    std::vector<std::string> header_;
};

constexpr int H = 24;
constexpr double CAP_PERCENTILE = 99.0;
constexpr double MIN_RIDERS = 1.0;
const double NaN = std::nan("");

using Hours = std::array<double, H>;

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}
size_t utf8Len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}
std::string strip0(const std::string& s) {
    size_t i = 0;
    while (i + 1 < s.size() && s[i] == '0' && std::isdigit(static_cast<unsigned char>(s[i + 1]))) ++i;
    return s.substr(i);
}
int daysInMonth(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return (m == 2 && leap) ? 29 : d[m - 1];
}

class RouteMatcher {
public:
    explicit RouteMatcher(const std::vector<std::string>& rideRoutes) : all_(rideRoutes) {
        for (const auto& r : rideRoutes) byNorm_[strip0(r)].push_back(r);
    }

    std::vector<std::string> match(const std::string& name) const {
        if (auto it = byNorm_.find(name); it != byNorm_.end()) return it->second;

        std::vector<std::string> out;
        for (const auto& [k, rs] : byNorm_)
            if (k.size() == name.size() + 1 && startsWith(k, name) && std::isupper(static_cast<unsigned char>(k.back())))
                out.insert(out.end(), rs.begin(), rs.end());
        if (!out.empty()) { std::sort(out.begin(), out.end()); return out; }

        const std::string* best = nullptr;
        for (const auto& [k, rs] : byNorm_)
            if (utf8Len(k) >= 3 && startsWith(name, k) && (!best || k.size() > best->size())) best = &k;
        if (best) return byNorm_.at(*best);

        for (const std::string suf : {"출근", "퇴근"}) {
            std::string num = name.substr(0, name.size() >= suf.size() ? name.size() - suf.size() : 0);
            if (endsWith(name, suf) && allDigits(num)) {
                std::string tag = suf == "출근" ? "출" : "퇴";
                for (const auto& r : all_) if (startsWith(r, num) && endsWith(r, tag)) out.push_back(r);
                return out;
            }
        }
        return {};
    }

private:
    std::vector<std::string> all_;
    std::map<std::string, std::vector<std::string>> byNorm_;
};

struct EdgeRow {
    int routeId, srcId, dstId, srcSeq, dstSeq;
    std::string routeName;
    double travelTime;
};

struct RideData {
    std::unordered_map<std::string, std::unordered_map<int, std::pair<Hours, Hours>>> onoff;
    std::vector<std::string> routes;
    int days = 30;
};

std::vector<EdgeRow> readEdges(const std::string& path) {
    CsvReader r(path);
    int cR = r.col("노선_ID"), cN = r.col("노선명"), cS = r.col("출발_정류장_ID"), cD = r.col("도착_정류장_ID"),
        cSq = r.col("출발_정류장_순서"), cDq = r.col("도착_정류장_순서"), cT = r.col("기본_이동시간_초");
    std::vector<EdgeRow> rows;
    std::vector<std::string> f;
    while (r.next(f)) {
        if (f.size() < r.header().size()) continue;
        rows.push_back({std::stoi(f[cR]), std::stoi(f[cS]), std::stoi(f[cD]),
                        std::stoi(f[cSq]), std::stoi(f[cDq]), f[cN], std::stod(f[cT])});
    }
    return rows;
}

RideData readRidership(const std::string& path) {
    CsvReader r(path);
    int cY = r.col("사용년월"), cR = r.col("노선번호"), cS = r.col("표준버스정류장ID");
    std::array<int, H> cb, ca;
    for (int h = 0; h < H; ++h) {
        std::string hh = h == 0 ? "00" : std::to_string(h);
        cb[h] = r.col(hh + "시승차총승객수");
        ca[h] = r.col(hh + "시하차총승객수");
    }
    RideData d;
    std::string ym;
    std::unordered_set<std::string> routeSet;
    std::vector<std::string> f;
    while (r.next(f)) {
        if (f.size() <= static_cast<size_t>(std::max(cb[H - 1], ca[H - 1]))) continue;
        if (ym.empty()) ym = f[cY];
        const std::string& route = f[cR];
        auto& [B, A] = d.onoff[route][std::stoi(f[cS])];
        for (int h = 0; h < H; ++h) { B[h] += std::stod(f[cb[h]]); A[h] += std::stod(f[ca[h]]); }
        if (routeSet.insert(route).second) d.routes.push_back(route);
    }
    std::sort(d.routes.begin(), d.routes.end());
    if (ym.size() >= 6) d.days = daysInMonth(std::stoi(ym.substr(0, 4)), std::stoi(ym.substr(4, 2)));
    return d;
}

struct LoadResult {
    std::vector<Hours> load;
    std::vector<std::array<bool, H>> running;
};

LoadResult computeLoads(const std::vector<EdgeRow>& edges, const RideData& ride) {
    size_t E = edges.size();
    LoadResult res;
    Hours nanRow;
    nanRow.fill(NaN);
    res.load.assign(E, nanRow);
    res.running.assign(E, {});

    std::vector<int> routeOrder;
    std::unordered_map<int, std::vector<int>> rowsOf;
    for (size_t i = 0; i < E; ++i) {
        auto& v = rowsOf[edges[i].routeId];
        if (v.empty()) routeOrder.push_back(edges[i].routeId);
        v.push_back(static_cast<int>(i));
    }

    RouteMatcher matcher(ride.routes);
    for (int rid : routeOrder) {
        const auto& rows = rowsOf[rid];
        auto cands = matcher.match(edges[rows[0]].routeName);
        if (cands.empty()) {
            for (int i : rows) res.running[i].fill(true);
            continue;
        }

        std::map<int, int> seq2stop;
        for (int i : rows) {
            seq2stop[edges[i].srcSeq] = edges[i].srcId;
            seq2stop[edges[i].dstSeq] = edges[i].dstId;
        }
        std::vector<int> order, stops;
        std::unordered_map<int, int> repeat;
        for (auto& [q, s] : seq2stop) { order.push_back(q); stops.push_back(s); ++repeat[s]; }

        size_t n = stops.size();
        std::vector<Hours> B(n), A(n);
        for (size_t i = 0; i < n; ++i) {
            B[i].fill(0);
            A[i].fill(0);
            for (const auto& r : cands) {
                auto rit = ride.onoff.find(r);
                if (rit == ride.onoff.end()) continue;
                auto sit = rit->second.find(stops[i]);
                if (sit == rit->second.end()) continue;
                for (int h = 0; h < H; ++h) {
                    B[i][h] += sit->second.first[h] / repeat[stops[i]];
                    A[i][h] += sit->second.second[h] / repeat[stops[i]];
                }
            }
        }

        Hours tb{}, ta{};
        double TB = 0, TA = 0;
        for (size_t i = 0; i < n; ++i)
            for (int h = 0; h < H; ++h) { tb[h] += B[i][h]; ta[h] += A[i][h]; }
        for (int h = 0; h < H; ++h) { TB += tb[h]; TA += ta[h]; }
        double dayF = TA > 0 ? TB / TA : 0.0;
        for (int h = 0; h < H; ++h) {
            double f = ta[h] > 0 ? tb[h] / ta[h] : dayF;
            for (size_t i = 0; i < n; ++i) A[i][h] *= f;
        }

        std::unordered_map<int, Hours> seqLoad;
        Hours cum{};
        for (size_t i = 0; i < n; ++i) {
            Hours l;
            for (int h = 0; h < H; ++h) {
                cum[h] += B[i][h] - A[i][h];
                l[h] = std::max(cum[h], 0.0) / ride.days;
            }
            seqLoad[order[i]] = l;
        }

        std::array<bool, H> routeRunning;
        for (int h = 0; h < H; ++h) routeRunning[h] = tb[h] / ride.days >= MIN_RIDERS;
        for (int i : rows) {
            res.load[i] = seqLoad[edges[i].srcSeq];
            res.running[i] = routeRunning;
        }
    }
    for (size_t i = 0; i < E; ++i)
        for (int h = 0; h < H; ++h)
            if (!res.running[i][h]) res.load[i][h] = NaN;
    return res;
}

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    double pos = p / 100.0 * (v.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(pos)), hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (pos - lo);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <bus_edges.csv> <ridership.csv> [out=congestion.csv]\n";
        return 1;
    }
    auto edges = readEdges(argv[1]);
    auto ride = readRidership(argv[2]);
    auto L = computeLoads(edges, ride);

    std::vector<double> vals;
    for (auto& row : L.load)
        for (double x : row)
            if (!std::isnan(x) && x > 0) vals.push_back(x);
    double cap = percentile(vals, CAP_PERCENTILE);

    std::ofstream out(argc > 3 ? argv[3] : "congestion.csv", std::ios::binary);
    out << "\xEF\xBB\xBF" << "route_id,route_name,src_id,dst_id,src_seq";
    for (int h = 0; h < H; ++h) {
        char buf[8];
        std::snprintf(buf, sizeof buf, ",c%02d", h);
        out << buf;
    }
    out << '\n';
    for (size_t i = 0; i < edges.size(); ++i) {
        const EdgeRow& e = edges[i];
        out << e.routeId << ',' << e.routeName << ',' << e.srcId << ',' << e.dstId << ',' << e.srcSeq;
        for (int h = 0; h < H; ++h) {
            out << ',';
            double l = L.load[i][h];
            if (!std::isnan(l)) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "%.4f", std::min(std::max(l / cap, 0.0), 1.0));
                out << buf;
            }
        }
        out << '\n';
    }
    return 0;
}
