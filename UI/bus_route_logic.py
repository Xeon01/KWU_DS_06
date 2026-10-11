"""데이터 로딩과 버스 경로 계산을 담당한다."""

import csv
import heapq
import io
import math
import zipfile
from functools import lru_cache
from pathlib import Path

import pandas as pd


PROJECT_DIR = Path(__file__).resolve().parent.parent
EDGE_FILE = PROJECT_DIR / "bus_edges_혼잡도.csv"
STOP_ZIP_FILE = PROJECT_DIR / "버스 노선 전처리에 사용한 파일.zip"
STOP_FILE_KEYWORD = "정류소정보"
CONGESTION_WEIGHT = 4.0

EDGE_COLUMNS = [
    "노선_ID",
    "노선명",
    "출발_정류장_ID",
    "도착_정류장_ID",
    "기본_이동시간_초",
] + [f"혼잡도_{hour:02d}시" for hour in range(24)]


@lru_cache(maxsize=1)
def load_data():
    """혼잡도 CSV와 ZIP 안의 정류장 이름·좌표를 읽는다."""
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


@lru_cache(maxsize=2)
def build_graph(hour):
    """선택한 시간대의 노선 데이터를 정류장별 인접 목록으로 만든다."""
    edges, stops = load_data()
    congestion_column = f"혼잡도_{hour:02d}시"
    graph = {}

    columns = [
        "노선_ID",
        "노선명",
        "출발_정류장_ID",
        "도착_정류장_ID",
        "기본_이동시간_초",
        congestion_column,
    ]

    for row in edges[columns].to_dict("records"):
        start_id = row["출발_정류장_ID"]
        end_id = row["도착_정류장_ID"]
        try:
            travel_seconds = float(row["기본_이동시간_초"])
        except (TypeError, ValueError):
            continue

        raw_congestion = row[congestion_column]
        congestion = None
        if pd.notna(raw_congestion) and str(raw_congestion).strip():
            congestion = float(raw_congestion)

        edge = {
            "route_id": row["노선_ID"],
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


def find_path(graph, start_id, end_id, avoid_congestion=False):
    """다익스트라 알고리즘으로 최단 거리 또는 혼잡 회피 경로를 찾는다."""
    distances = {start_id: 0.0}
    previous_edges = {}
    queue = [(0.0, start_id)]

    while queue:
        current_cost, current_id = heapq.heappop(queue)
        if current_cost > distances.get(current_id, float("inf")):
            continue
        if current_id == end_id:
            break

        for edge in graph.get(current_id, []):
            if avoid_congestion:
                congestion = edge["congestion"] or 0.0
                edge_cost = edge["travel_seconds"] * (
                    1 + CONGESTION_WEIGHT * congestion
                )
            else:
                edge_cost = edge["distance_km"]
                if edge_cost is None:
                    continue

            next_cost = current_cost + edge_cost
            next_id = edge["end_id"]
            if next_cost < distances.get(next_id, float("inf")):
                distances[next_id] = next_cost
                previous_edges[next_id] = edge
                heapq.heappush(queue, (next_cost, next_id))

    if end_id not in distances:
        return None

    path = []
    current_id = end_id
    while current_id != start_id:
        edge = previous_edges[current_id]
        path.append(edge)
        current_id = edge["start_id"]
    path.reverse()
    return path


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


def group_legs(path):
    """같은 버스를 연속으로 타는 구간을 하나로 묶는다."""
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
    for number, leg in enumerate(legs, start=1):
        congestion = (
            f"{sum(leg['congestions']) / len(leg['congestions']) * 100:.1f}%"
            if leg["congestions"]
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
                "혼잡도": congestion,
            }
        )
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
