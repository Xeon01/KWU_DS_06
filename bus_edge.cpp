#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <utility>

using namespace std;
namespace fs = std::filesystem;

using Row = vector<string>;

// 노선 ID, 출발 정류장 ID, 도착 정류장 ID,
// 출발 순번, 도착 순번
using EdgeKey = array<string, 5>;

struct Stop {
    string id;
    string name;
    string longitude;
    string latitude;
};

// 따옴표 안의 쉼표, 줄바꿈, ""를 처리하는 CSV 읽기 함수
bool readCsvRow(istream& input, Row& row) {
    row.clear();

    string field;
    bool inQuotes = false;
    bool hasInput = false;
    char ch;

    while (input.get(ch)) {
        hasInput = true;

        if (inQuotes) {
            if (ch == '"') {
                if (input.peek() == '"') {
                    input.get(ch);
                    field += '"';
                }
                else {
                    inQuotes = false;
                }
            }
            else {
                field += ch;
            }
        }
        else {
            if (ch == '"' && field.empty()) {
                inQuotes = true;
            }
            else if (ch == ',') {
                row.push_back(field);
                field.clear();
            }
            else if (ch == '\n' || ch == '\r') {
                if (ch == '\r' && input.peek() == '\n') {
                    input.get(ch);
                }

                row.push_back(field);
                return true;
            }
            else {
                field += ch;
            }
        }
    }

    if (inQuotes) {
        throw runtime_error("CSV 따옴표가 닫히지 않았습니다.");
    }

    if (!hasInput) {
        return false;
    }

    row.push_back(field);
    return true;
}

// CSV 파일과 헤더 관리
class CsvReader {
private:
    ifstream file;
    map<string, size_t> columnIndex;
    size_t columnCount = 0;

public:
    explicit CsvReader(const fs::path& path)
        : file(path, ios::binary) {

        if (!file) {
            throw runtime_error(
                "파일을 열 수 없습니다: " + path.string()
            );
        }

        // UTF-8 BOM이 있으면 건너뛴다.
        char bom[3] = {};
        file.read(bom, 3);

        bool hasBom =
            file.gcount() == 3 &&
            static_cast<unsigned char>(bom[0]) == 0xEF &&
            static_cast<unsigned char>(bom[1]) == 0xBB &&
            static_cast<unsigned char>(bom[2]) == 0xBF;

        if (!hasBom) {
            file.clear();
            file.seekg(0);
        }

        Row header;

        if (!readCsvRow(file, header)) {
            throw runtime_error("CSV 헤더가 없습니다.");
        }

        columnCount = header.size();

        for (size_t i = 0; i < header.size(); ++i) {
            columnIndex[header[i]] = i;
        }
    }

    bool next(Row& row) {
        while (readCsvRow(file, row)) {
            // 완전히 빈 줄은 건너뛴다.
            if (row.size() == 1 && row[0].empty()) {
                continue;
            }

            if (row.size() != columnCount) {
                throw runtime_error("CSV 열 개수가 맞지 않습니다.");
            }

            return true;
        }

        return false;
    }

    const string& get(
        const Row& row,
        const string& column
    ) const {
        auto it = columnIndex.find(column);

        if (it == columnIndex.end()) {
            throw runtime_error("없는 열 이름: " + column);
        }

        return row.at(it->second);
    }
};

// 쉼표, 따옴표, 줄바꿈이 있는 값은 따옴표로 감싼다.
void writeCsvRow(ostream& output, const Row& row) {
    for (size_t i = 0; i < row.size(); ++i) {
        if (i > 0) {
            output << ',';
        }

        const string& value = row[i];

        if (value.find_first_of(",\"\r\n") == string::npos) {
            output << value;
        }
        else {
            output << '"';

            for (char ch : value) {
                if (ch == '"') {
                    output << "\"\"";
                }
                else {
                    output << ch;
                }
            }

            output << '"';
        }
    }

    output << "\r\n";
}

// 원본 값을 초(s)로 해석한다.
// 별도의 단위 환산은 하지 않고, 0만 빈칸으로 바꾼다.
string normalizeTime(const string& value) {
    if (value.empty()) {
        return "";
    }

    size_t parsed = 0;
    double number = stod(value, &parsed);

    if (parsed != value.size() || number < 0) {
        throw runtime_error("잘못된 운행시간: " + value);
    }

    return number == 0 ? "" : value;
}

int main(int argc, char* argv[]) {
    try {
        // 인자가 없으면 현재 작업 폴더에서 CSV를 읽는다.
        fs::path folder =
            argc >= 2 ? fs::u8path(argv[1]) : fs::current_path();

        fs::path routePath =
            folder / fs::u8path(
                "서울시버스노선ID정보(20260902).csv"
            );

        fs::path stopPath =
            folder / fs::u8path(
                "서울시버스노선별정류소정보(20260902).csv"
            );

        fs::path timePath =
            folder / fs::u8path(
                "tpss_route_section_speedh_2026.09.14-09.20.csv"
            );

        fs::path outputPath =
            folder / "bus_edges.csv";

        // 노선 ID -> 노선명
        map<string, string> routeNames;

        // 정류장 ID -> 정류장 정보
        map<string, Stop> stops;

        // 노선 ID -> (순번 -> 정류장 ID)
        map<string, map<int, string>> routeStops;

        Row row;

        // 1. 노선 정보 읽기
        {
            CsvReader reader(routePath);

            while (reader.next(row)) {
                routeNames[reader.get(row, "ROUTEID")] =
                    reader.get(row, "노선명");
            }
        }

        // 2. 정류장 정보 읽기
        {
            CsvReader reader(stopPath);

            while (reader.next(row)) {
                string routeId = reader.get(row, "ROUTE_ID");
                string stopId = reader.get(row, "NODE_ID");
                int sequence = stoi(reader.get(row, "순번"));

                stops[stopId] = {
                    stopId,
                    reader.get(row, "정류소명"),
                    reader.get(row, "X좌표"),
                    reader.get(row, "Y좌표")
                };

                routeStops[routeId][sequence] = stopId;

                // 노선 ID 파일에 없으면 정류장 파일의 노선명 사용
                if (routeNames.find(routeId) == routeNames.end()) {
                    routeNames[routeId] =
                        reader.get(row, "노선명");
                }
            }
        }

        // 3. 노선에서 순번이 연속인 정류장을 방향 간선으로 연결
        set<EdgeKey> topologyEdges;

        for (const auto& route : routeStops) {
            const string& routeId = route.first;
            const auto& orderedStops = route.second;

            for (const auto& current : orderedStops) {
                int sequence = current.first;
                auto next = orderedStops.find(sequence + 1);

                if (next == orderedStops.end()) {
                    continue;
                }

                topologyEdges.insert({
                    routeId,
                    current.second,
                    next->second,
                    to_string(sequence),
                    to_string(sequence + 1)
                    });
            }
        }

        // 4. 입력·출력 시간 열 이름 구성
        vector<string> sourceTimeColumns = { "운행시간" };

        Row header = {
            "기준_날짜",
            "노선_ID",
            "노선명",
            "출발_정류장_ID",
            "출발_정류장명",
            "출발_경도",
            "출발_위도",
            "출발_정류장_순서",
            "도착_정류장_ID",
            "도착_정류장명",
            "도착_경도",
            "도착_위도",
            "도착_정류장_순서",
            "이동시간_s"
        };

        for (int hour = 0; hour < 24; ++hour) {
            string hh =
                (hour < 10 ? string("0") : string("")) +
                to_string(hour);

            sourceTimeColumns.push_back(
                "운행시간_" + hh + "시"
            );

            header.push_back(
                "이동시간_" + hh + "시_s"
            );
        }

        ofstream output(outputPath, ios::binary);

        if (!output) {
            throw runtime_error("출력 파일을 만들 수 없습니다.");
        }

        // Excel에서 한글을 인식할 수 있도록 UTF-8 BOM 기록
        output.write("\xEF\xBB\xBF", 3);
        writeCsvRow(output, header);

        size_t outputCount = 0;

        // 정류장 이름·좌표 조회. 없으면 빈 문자열 유지.
        auto lookupStop = [&](const string& id) -> Stop {
            auto it = stops.find(id);

            if (it != stops.end()) {
                return it->second;
            }

            return { id, "", "", "" };
            };

        // 간선 한 행 출력
        auto writeEdge = [&](
            const EdgeKey& key,
            const string& date,
            const Row& times
            ) {
                const string& routeId = key[0];

                Stop from = lookupStop(key[1]);
                Stop to = lookupStop(key[2]);

                auto routeIt = routeNames.find(routeId);
                string routeName =
                    routeIt == routeNames.end()
                    ? ""
                    : routeIt->second;

                Row result = {
                    date,
                    routeId,
                    routeName,
                    from.id,
                    from.name,
                    from.longitude,
                    from.latitude,
                    key[3],
                    to.id,
                    to.name,
                    to.longitude,
                    to.latitude,
                    key[4]
                };

                result.insert(
                    result.end(),
                    times.begin(),
                    times.end()
                );

                writeCsvRow(output, result);
                ++outputCount;
            };

        // 운행시간 파일에 등장한 간선과 정류장
        set<EdgeKey> observedEdges;
        set<string> usedStops;

        // 5. 운행시간 데이터와 정류장·노선 정보를 결합
        {
            CsvReader reader(timePath);

            while (reader.next(row)) {
                EdgeKey key = {
                    reader.get(row, "노선_ID"),
                    reader.get(row, "출발_정류장_ID"),
                    reader.get(row, "도착_정류장_ID"),
                    reader.get(row, "출발_정류장_순서"),
                    reader.get(row, "도착_정류장_순서")
                };

                Row times;

                for (const string& column : sourceTimeColumns) {
                    times.push_back(
                        normalizeTime(reader.get(row, column))
                    );
                }

                writeEdge(
                    key,
                    reader.get(row, "기준_날짜"),
                    times
                );

                observedEdges.insert(key);
                usedStops.insert(key[1]);
                usedStops.insert(key[2]);
            }
        }

        // 6. 정류장 순서에는 있지만 운행시간이 없는 간선 추가
        // 날짜와 25개 시간 값은 빈칸으로 둔다.
        Row emptyTimes(25, "");

        for (const EdgeKey& key : topologyEdges) {
            if (observedEdges.find(key) == observedEdges.end()) {
                writeEdge(key, "", emptyTimes);

                usedStops.insert(key[1]);
                usedStops.insert(key[2]);
            }
        }

        // 7. 간선에 포함되지 않은 정류장도 보존
        // 현재 제공된 데이터에서는 이러한 정류장이 없었다.
        for (const auto& entry : stops) {
            const string& stopId = entry.first;

            if (usedStops.find(stopId) == usedStops.end()) {
                EdgeKey key = { "", stopId, "", "", "" };
                writeEdge(key, "", emptyTimes);
            }
        }

        output.close();

        if (!output) {
            throw runtime_error("파일 저장 중 오류가 발생했습니다.");
        }

        cout << "생성 완료: "
            << outputPath.string() << '\n';
        cout << "데이터 행 수: " << outputCount << '\n';
        cout << "열 수: " << header.size() << '\n';
    }
    catch (const exception& error) {
        cerr << "오류: " << error.what() << '\n';
        return 1;
    }

    return 0;
}