"""데이터 로딩과 C++ Pathfinder 기반 버스 경로 계산을 담당한다."""

import csv
import io
import json
import math
import os
import re
import subprocess
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from functools import lru_cache
from pathlib import Path

try:
    import pandas as pd
except ImportError:
    pd = None


PROJECT_DIR = Path(__file__).resolve().parent.parent


def _resolve_data_file(filename):
    for candidate in (PROJECT_DIR / filename, PROJECT_DIR / "Postprocessing" / filename):
        if candidate.exists():
            return candidate
    return PROJECT_DIR / filename


EDGE_FILE = _resolve_data_file("bus_edges_혼잡도.csv")
STOP_ZIP_FILE = _resolve_data_file("버스 노선 전처리에 사용한 파일.zip")
STOP_FILE_KEYWORD = "정류소정보"
SEARCH_DIR = PROJECT_DIR / "Search"
GRAPH_DIR = SEARCH_DIR / "graphs"
CONGESTION_WEIGHT = 4.0

EDGE_COLUMNS = [
    "노선_ID",
    "노선명",
    "출발_정류장_ID",
    "도착_정류장_ID",
    "기본_이동시간_초",
] + [f"혼잡도_{hour:02d}시" for hour in range(24)]


def _is_present(value):
    if value is None:
        return False
    if pd is not None and pd.isna(value):
        return False
    return bool(str(value).strip())


@lru_cache(maxsize=1)
def load_stops():
    """ZIP 파일 안의 정류장 이름·ARS 번호·좌표를 읽는다."""
    with zipfile.ZipFile(STOP_ZIP_FILE) as zip_file:
        stop_file = next(
            name for name in zip_file.namelist()
            if STOP_FILE_KEYWORD in name and name.endswith(".csv")
        )
        with zip_file.open(stop_file) as raw_file:
            reader = csv.DictReader(io.TextIOWrapper(raw_file, encoding="utf-8-sig"))
            stops = {}
            for row in reader:
                stop_id = row["NODE_ID"]
                if stop_id in stops:
                    continue
                try:
                    stops[stop_id] = {
                        "name": row["정류소명"].strip(),
                        "ars_id": row["ARS_ID"].strip(),
                        "latitude": float(row["Y좌표"]),
                        "longitude": float(row["X좌표"]),
                    }
                except (TypeError, ValueError):
                    continue
    return stops


@lru_cache(maxsize=1)
def load_data():
    """혼잡도 CSV와 ZIP 안의 정류장 이름·좌표를 읽는다."""
    stops = load_stops()
    if pd is not None:
        edges = pd.read_csv(
            EDGE_FILE,
            encoding="cp949",
            usecols=EDGE_COLUMNS,
            dtype={
                "노선_ID": str,
                "출발_정류장_ID": str,
                "도착_정류장_ID": str,
            },
        )
        return edges, stops

    with open(EDGE_FILE, "r", encoding="cp949", newline="") as raw_file:
        edges = list(csv.DictReader(raw_file))
    return edges, stops


def distance_between(start_stop, end_stop):
    """두 정류장 좌표 사이의 직선거리를 km로 계산한다."""
    if not start_stop or not end_stop:
        return None

    earth_radius_km = 6371.0
    lat1 = math.radians(start_stop["latitude"])
    lat2 = math.radians(end_stop["latitude"])
    lat_delta = lat2 - lat1
    lon_delta = math.radians(end_stop["longitude"] - start_stop["longitude"])

    value = (
        math.sin(lat_delta / 2) ** 2
        + math.cos(lat1) * math.cos(lat2) * math.sin(lon_delta / 2) ** 2
    )
    return 2 * earth_radius_km * math.asin(math.sqrt(value))


def parse_coordinates(text):
    """'위도, 경도' 또는 '위도 경도' 문자열에서 좌표를 파싱한다(경도, 위도 순서 입력도 보정)."""
    if not text:
        return None
    parts = [p for p in re.split(r"[\s,]+", text.strip()) if p]
    if len(parts) != 2:
        return None
    try:
        a = float(parts[0])
        b = float(parts[1])
    except ValueError:
        return None

    if 33.0 <= a <= 39.0 and 124.0 <= b <= 132.0:
        return a, b
    if 124.0 <= a <= 132.0 and 33.0 <= b <= 39.0:
        return b, a
    return None


@lru_cache(maxsize=64)
def _geocode_nominatim_cached(query):
    """OpenStreetMap Nominatim API로 장소/주소를 좌표로 변환한다."""
    params = urllib.parse.urlencode(
        {
            "q": query,
            "format": "json",
            "limit": 1,
            "countrycodes": "kr",
        }
    )
    url = f"https://nominatim.openstreetmap.org/search?{params}"
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "KWU_DS_06_BusRouteApp/1.0"},
    )
    with urllib.request.urlopen(req, timeout=3.0) as resp:
        payload = json.loads(resp.read().decode("utf-8"))
    if not payload:
        return None
    top = payload[0]
    return {
        "latitude": float(top["lat"]),
        "longitude": float(top["lon"]),
        "display_name": top.get("display_name", query),
    }


def geocode_place(query):
    """장소/주소를 좌표로 변환한다. 실패 시 None을 반환한다."""
    cleaned = (query or "").strip()
    if not cleaned:
        return None
    try:
        return _geocode_nominatim_cached(cleaned)
    except (urllib.error.URLError, TimeoutError, ValueError, KeyError, OSError):
        return None


def find_nearest_stop(latitude, longitude, stops, candidate_ids=None):
    """주어진 위도·경도에서 가장 가까운 버스 정류장 ID와 거리(km)를 찾는다."""
    target = {"latitude": latitude, "longitude": longitude}
    ids = candidate_ids if candidate_ids is not None else stops.keys()
    best_id = None
    best_dist = float("inf")

    for stop_id in ids:
        stop = stops.get(stop_id)
        if not stop:
            continue
        dist = distance_between(target, stop)
        if dist is not None and dist < best_dist:
            best_dist = dist
            best_id = stop_id

    if best_id is None:
        return None, None
    return best_id, best_dist


def find_stop_by_name(query, stops, candidate_ids=None):
    """정류장명 또는 정류장 번호(ARS ID)로 가장 잘 맞는 정류장을 찾는다."""
    cleaned = (query or "").strip()
    if not cleaned:
        return None
    q_lower = cleaned.lower()
    tokens = [t for t in q_lower.split() if t]
    ids = candidate_ids if candidate_ids is not None else stops.keys()

    best_score = None
    best_id = None
    for stop_id in ids:
        stop = stops.get(stop_id)
        if not stop:
            continue
        name = stop["name"]
        name_lower = name.lower()
        ars_id = (stop.get("ars_id") or "").lower()

        if name_lower == q_lower or ars_id == q_lower or str(stop_id).lower() == q_lower:
            score = (0, len(name))
        elif name_lower.startswith(q_lower):
            score = (1, len(name))
        elif q_lower in name_lower:
            score = (2, name_lower.index(q_lower), len(name))
        elif tokens and all(tok in name_lower for tok in tokens):
            score = (3, len(name))
        else:
            continue

        if best_score is None or score < best_score:
            best_score = score
            best_id = stop_id

    return best_id


def resolve_place_to_stop(query, stops, candidate_ids=None):
    """출발/도착지 입력(좌표·장소명·정류장명)을 가장 가까운 버스 정류장으로 변환한다."""
    cleaned = (query or "").strip()
    if not cleaned:
        return None

    coords = parse_coordinates(cleaned)
    if coords is not None:
        lat, lon = coords
        stop_id, dist_km = find_nearest_stop(lat, lon, stops, candidate_ids)
        if stop_id is not None:
            return {
                "stop_id": stop_id,
                "distance_km": dist_km,
                "method": "coordinates",
                "place_name": f"{lat:.5f}, {lon:.5f}",
            }

    # 정확한 정류장명/번호가 직접 일치하면 우선 매칭하고, 그 외에는 지오코딩 → 이름 검색 순으로 탐색
    exact_id = None
    q_lower = cleaned.lower()
    ids = candidate_ids if candidate_ids is not None else stops.keys()
    for sid in ids:
        stop = stops.get(sid)
        if stop and (stop["name"].lower() == q_lower or (stop.get("ars_id") or "").lower() == q_lower):
            exact_id = sid
            break
    if exact_id is not None:
        return {
            "stop_id": exact_id,
            "distance_km": 0.0,
            "method": "exact_stop",
            "place_name": stops[exact_id]["name"],
        }

    geo = geocode_place(cleaned)
    if geo is not None:
        stop_id, dist_km = find_nearest_stop(
            geo["latitude"], geo["longitude"], stops, candidate_ids
        )
        if stop_id is not None:
            return {
                "stop_id": stop_id,
                "distance_km": dist_km,
                "method": "geocode",
                "place_name": geo["display_name"],
            }

    fallback_id = find_stop_by_name(cleaned, stops, candidate_ids)
    if fallback_id is not None:
        return {
            "stop_id": fallback_id,
            "distance_km": None,
            "method": "name_fallback",
            "place_name": stops[fallback_id]["name"],
        }

    return None


@lru_cache(maxsize=2)
def build_graph(hour):
    """선택한 시간대의 노선 데이터를 정류장별 인접 목록으로 만든다."""
    edges, stops = load_data()
    congestion_column = f"혼잡도_{hour:02d}시"
    graph = {}

    if pd is not None and isinstance(edges, pd.DataFrame):
        columns = [
            "노선_ID",
            "노선명",
            "출발_정류장_ID",
            "도착_정류장_ID",
            "기본_이동시간_초",
            congestion_column,
        ]
        records = edges[columns].to_dict("records")
    else:
        records = edges

    for row in records:
        start_id = row["출발_정류장_ID"]
        end_id = row["도착_정류장_ID"]
        try:
            travel_seconds = float(row["기본_이동시간_초"])
        except (TypeError, ValueError):
            continue

        raw_congestion = row.get(congestion_column)
        congestion = float(raw_congestion) if _is_present(raw_congestion) else None

        edge = {
            "route_id": str(row["노선_ID"]),
            "route_name": str(row["노선명"]),
            "start_id": start_id,
            "end_id": end_id,
            "start_name": stops.get(start_id, {}).get("name", start_id),
            "end_name": stops.get(end_id, {}).get("name", end_id),
            "travel_seconds": travel_seconds,
            "congestion": congestion,
            "distance_km": distance_between(stops.get(start_id), stops.get(end_id)),
        }
        graph.setdefault(start_id, []).append(edge)

    return graph


def ensure_pathfinder_binary():
    """Search/Pathfinder.cpp 실행 파일을 찾거나 필요 시 g++로 정적 링크 컴파일한다."""
    exe_name = "pathfinder.exe" if os.name == "nt" else "pathfinder"
    local_bin = SEARCH_DIR / exe_name
    cached_bin = Path(tempfile.gettempdir()) / f"kwu_ds_06_static_{exe_name}"
    sources = [
        Path(__file__),
        SEARCH_DIR / "Datastructure.hpp",
        SEARCH_DIR / "Graph.hpp",
        SEARCH_DIR / "Graph.cpp",
        SEARCH_DIR / "Pathfinder.cpp",
    ]
    latest_src_mtime = max(src.stat().st_mtime for src in sources)

    for candidate in (local_bin, cached_bin):
        if candidate.exists() and candidate.stat().st_mtime >= latest_src_mtime:
            return candidate

    cmd = [
        "g++",
        "-std=c++17",
        "-O2",
    ]
    if os.name == "nt":
        cmd.append("-static")
    cmd.extend([
        str(SEARCH_DIR / "Graph.cpp"),
        str(SEARCH_DIR / "Pathfinder.cpp"),
        "-o",
        str(cached_bin),
    ])
    result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode != 0:
        raise RuntimeError(f"Pathfinder 컴파일 실패:\n{result.stderr}")
    return cached_bin


def find_nearby_stop_ids(stop_id, stops, candidate_ids=None, max_dist_km=0.6, limit=6):
    """선택한 정류장을 우선으로 두고, 도보 이동 가능한 반경 내 인접 정류장 ID 목록을 반환한다."""
    base = stops.get(stop_id)
    if not base:
        return [str(stop_id)]

    ids = candidate_ids if candidate_ids is not None else stops.keys()
    nearby = []
    for sid in ids:
        if sid == stop_id:
            continue
        other = stops.get(sid)
        if not other:
            continue
        dist = distance_between(base, other)
        if dist is not None and dist <= max_dist_km:
            nearby.append((dist, sid))
    nearby.sort(key=lambda item: item[0])
    return [str(stop_id)] + [str(sid) for _, sid in nearby[: max(0, limit - 1)]]


def find_paths_with_pathfinder(
    start_id,
    end_id,
    hour,
    minute=0,
    alpha=CONGESTION_WEIGHT,
    stops=None,
    candidate_ids=None,
    allow_nearby_fallback=True,
    include_extreme=True,
):
    """C++ Pathfinder를 호출해 최단 시간 경로(alpha=0), 혼잡 회피 경로(alpha), 극단적 회피 경로를 계산한다."""
    if stops is None:
        stops = load_stops()

    if allow_nearby_fallback:
        src_arg = ",".join(find_nearby_stop_ids(str(start_id), stops, candidate_ids))
        dst_arg = ",".join(find_nearby_stop_ids(str(end_id), stops, candidate_ids))
    else:
        src_arg = str(start_id)
        dst_arg = str(end_id)

    binary = ensure_pathfinder_binary()
    with tempfile.NamedTemporaryFile(
        prefix="pathfinder_", suffix=".csv", delete=False
    ) as tmp:
        out_csv = Path(tmp.name)

    try:
        cmd = [
            str(binary),
            str(GRAPH_DIR),
            str(alpha),
            "nonexistent_stops.csv",
            "--query",
            str(int(hour)),
            str(int(minute)),
            src_arg,
            dst_arg,
            str(out_csv),
        ]
        if include_extreme:
            cmd.append("--extreme")
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        if proc.returncode != 0:
            # 그래프에 정류장이 없거나 탐색 불가 시 UI에서 경로 없음으로 처리할 수 있도록 확인
            err_text = (proc.stderr or proc.stdout).strip()
            if "존재하지 않습니다" in err_text:
                return {
                    "shortest_path": None,
                    "less_crowded_path": None,
                    "extreme_path": None,
                    "actual_start_id": str(start_id),
                    "actual_end_id": str(end_id),
                }
            raise RuntimeError(f"Pathfinder 실행 실패 (code={proc.returncode}): {err_text}")

        fast_path = []
        comfy_path = []
        extreme_path = []
        if out_csv.exists():
            with open(out_csv, "r", encoding="utf-8-sig", newline="") as f:
                for row in csv.DictReader(f):
                    row_alpha = float(row["alpha"])
                    s_id = str(row["src_id"])
                    e_id = str(row["dst_id"])
                    raw_cong = row.get("congestion", "").strip()
                    edge = {
                        "route_id": str(row["route_id"]),
                        "route_name": row["route_name"],
                        "start_id": s_id,
                        "end_id": e_id,
                        "start_name": stops.get(s_id, {}).get("name", s_id),
                        "end_name": stops.get(e_id, {}).get("name", e_id),
                        "travel_seconds": float(row["travel_time"]),
                        "congestion": float(raw_cong) if raw_cong else None,
                        "distance_km": distance_between(stops.get(s_id), stops.get(e_id)),
                        "hour": int(row["hour"]),
                    }
                    if row_alpha < 0.0:
                        extreme_path.append(edge)
                    elif row_alpha == 0.0:
                        fast_path.append(edge)
                    else:
                        comfy_path.append(edge)

        chosen_path = comfy_path or fast_path or extreme_path
        actual_start = chosen_path[0]["start_id"] if chosen_path else str(start_id)
        actual_end = chosen_path[-1]["end_id"] if chosen_path else str(end_id)

        return {
            "shortest_path": fast_path or None,
            "less_crowded_path": (comfy_path or fast_path) if alpha > 0.0 and (comfy_path or fast_path) else (comfy_path or None),
            "extreme_path": extreme_path or None,
            "actual_start_id": actual_start,
            "actual_end_id": actual_end,
        }
    finally:
        try:
            out_csv.unlink(missing_ok=True)
        except OSError:
            pass


def find_path(graph, start_id, end_id, avoid_congestion=False, hour=0, minute=0):
    """호환용 래퍼: C++ Pathfinder를 호출해 단일 경로를 반환한다."""
    alpha = CONGESTION_WEIGHT if avoid_congestion else 0.0
    paths = find_paths_with_pathfinder(
        start_id=start_id,
        end_id=end_id,
        hour=hour,
        minute=minute,
        alpha=alpha if alpha > 0.0 else 0.0,
    )
    return paths["less_crowded_path"] if avoid_congestion else paths["shortest_path"]


def path_summary(path):
    """경로의 거리, 예상시간, 환승 횟수와 혼잡도를 계산한다."""
    if not path:
        return {
            "distance_km": 0.0,
            "minutes": 0.0,
            "transfers": 0,
            "congestion": None,
        }

    measured_distances = [
        edge["distance_km"] for edge in path if edge["distance_km"] is not None
    ]
    route_ids = []
    weighted_congestion = 0.0
    measured_time = 0.0

    for edge in path:
        if not route_ids or route_ids[-1] != edge["route_id"]:
            route_ids.append(edge["route_id"])
        if edge["congestion"] is not None:
            weighted_congestion += edge["congestion"] * edge["travel_seconds"]
            measured_time += edge["travel_seconds"]

    average_congestion = (
        weighted_congestion / measured_time if measured_time else None
    )
    return {
        "distance_km": sum(measured_distances),
        "minutes": sum(edge["travel_seconds"] for edge in path) / 60,
        "transfers": max(0, len(route_ids) - 1),
        "congestion": average_congestion,
    }


CONGESTION_COLOR_BANDS = [
    (0.2, "0~20%", "#2563EB", "#DBEAFE", "#1E3A8A"),    # 파랑
    (0.4, "20~40%", "#16A34A", "#DCFCE7", "#14532D"),   # 초록
    (0.6, "40~60%", "#EAB308", "#FEF9C3", "#713F12"),   # 노랑
    (0.8, "60~80%", "#F97316", "#FFEDD5", "#7C2D12"),   # 주황
    (1.01, "80~100%", "#DC2626", "#FEE2E2", "#7F1D1D"), # 빨강
]


def congestion_color_info(congestion):
    """혼잡도(0~1)를 20% 단위 색상(파랑/초록/노랑/주황/빨강)으로 매핑한다."""
    if congestion is None:
        return {
            "label": "자료 없음",
            "line_color": "#6B7280",
            "bg_color": "#F3F4F6",
            "text_color": "#374151",
        }
    value = min(max(float(congestion), 0.0), 1.0)
    for upper, label, line_color, bg_color, text_color in CONGESTION_COLOR_BANDS:
        if value < upper:
            return {
                "label": label,
                "line_color": line_color,
                "bg_color": bg_color,
                "text_color": text_color,
            }
    _, label, line_color, bg_color, text_color = CONGESTION_COLOR_BANDS[-1]
    return {
        "label": label,
        "line_color": line_color,
        "bg_color": bg_color,
        "text_color": text_color,
    }


def group_legs_with_styles(path):
    """같은 버스를 연속으로 타는 구간을 묶고 구간별 혼잡도 색상 정보를 함께 반환한다."""
    legs = []
    for edge in path:
        if not legs or legs[-1]["route_id"] != edge["route_id"]:
            legs.append(
                {
                    "route_id": edge["route_id"],
                    "route_name": edge["route_name"],
                    "start_name": edge["start_name"],
                    "end_name": edge["end_name"],
                    "travel_seconds": edge["travel_seconds"],
                    "distance_km": edge["distance_km"] or 0.0,
                    "congestions": [],
                }
            )
        else:
            legs[-1]["end_name"] = edge["end_name"]
            legs[-1]["travel_seconds"] += edge["travel_seconds"]
            legs[-1]["distance_km"] += edge["distance_km"] or 0.0

        if edge["congestion"] is not None:
            legs[-1]["congestions"].append(edge["congestion"])

    rows = []
    styles = []
    for number, leg in enumerate(legs, start=1):
        avg_congestion = (
            sum(leg["congestions"]) / len(leg["congestions"])
            if leg["congestions"]
            else None
        )
        congestion_text = (
            f"{avg_congestion * 100:.1f}%"
            if avg_congestion is not None
            else "자료 없음"
        )
        rows.append(
            {
                "구간": number,
                "버스 노선": leg["route_name"],
                "승차 정류장": leg["start_name"],
                "하차 정류장": leg["end_name"],
                "예상 시간 (분)": round(leg["travel_seconds"] / 60, 1),
                "거리 (km)": round(leg["distance_km"], 2),
                "혼잡도": congestion_text,
            }
        )
        styles.append(congestion_color_info(avg_congestion))
    return rows, styles


def group_legs(path):
    """같은 버스를 연속으로 타는 구간을 하나로 묶는다."""
    rows, _ = group_legs_with_styles(path)
    return rows


def crowded_stops(graph, stops):
    """선택한 시간대에 연결 구간 혼잡도가 높은 정류장을 모은다."""
    congestion_sum = {}
    edge_count = {}

    for start_id, outgoing_edges in graph.items():
        for edge in outgoing_edges:
            if edge["congestion"] is None:
                continue
            for stop_id in (start_id, edge["end_id"]):
                congestion_sum[stop_id] = congestion_sum.get(stop_id, 0.0) + edge["congestion"]
                edge_count[stop_id] = edge_count.get(stop_id, 0) + 1

    ranked = []
    for stop_id, total in congestion_sum.items():
        if stop_id in stops and edge_count[stop_id]:
            ranked.append(
                {
                    "정류장": stops[stop_id]["name"],
                    "정류장 번호": stops[stop_id]["ars_id"] or stop_id,
                    "평균 혼잡도": total / edge_count[stop_id],
                }
            )
    ranked.sort(key=lambda row: row["평균 혼잡도"], reverse=True)
    return ranked[:10]
