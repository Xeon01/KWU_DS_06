"""Python GUI 로직(UI/bus_route_logic.py)과 C++ Pathfinder 연동을 검증한다."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "UI"))

import bus_route_logic  # noqa: E402


def main():
    for path in (ROOT / "UI" / "bus_route_logic.py", ROOT / "UI" / "dashboard.py"):
        compile(path.read_text(encoding="utf-8"), str(path), "exec")

    stops = bus_route_logic.load_stops()
    assert len(stops) > 1000, f"Expected stops loaded, got {len(stops)}"

    # 미운행 간선(TOUR11: 전 시간대 혼잡도 미집계)은 탐색하지 않아야 함
    non_operating = bus_route_logic.find_paths_with_pathfinder(
        start_id="101000308",
        end_id="101000311",
        hour=8,
        minute=15,
        alpha=bus_route_logic.CONGESTION_WEIGHT,
        stops=stops,
        allow_nearby_fallback=False,
    )
    assert non_operating["shortest_path"] is None
    assert non_operating["less_crowded_path"] is None
    print("[PASS] Non-operating route (TOUR11) skipped")

    # 정류장 좌표 및 혼잡도 데이터가 있는 실제 서울 시내 구간 검증 (우이동도선사입구 -> 동묘앞)
    seoul_res = bus_route_logic.find_paths_with_pathfinder(
        start_id="108000014",
        end_id="100000366",
        hour=8,
        minute=15,
        alpha=bus_route_logic.CONGESTION_WEIGHT,
        stops=stops,
    )
    seoul_fast = seoul_res["shortest_path"]
    seoul_comfy = seoul_res["less_crowded_path"]
    seoul_extreme = seoul_res["extreme_path"]
    assert seoul_fast is not None and len(seoul_fast) > 0
    assert seoul_comfy is not None and len(seoul_comfy) > 0
    assert seoul_extreme is not None and len(seoul_extreme) > 0
    assert all(edge["congestion"] is not None for edge in seoul_fast)
    assert all(edge["congestion"] is not None for edge in seoul_comfy)
    assert all(edge["congestion"] is not None for edge in seoul_extreme)
    s_fast = bus_route_logic.path_summary(seoul_fast)
    s_comfy = bus_route_logic.path_summary(seoul_comfy)
    s_extreme = bus_route_logic.path_summary(seoul_extreme)
    assert s_fast["distance_km"] > 0.0
    assert s_fast["congestion"] is not None
    assert s_extreme["congestion"] <= s_comfy["congestion"] + 1e-9
    print(f"[PASS] Seoul route fast: {len(seoul_fast)} edges, {s_fast['minutes']:.1f} min, {s_fast['distance_km']:.2f} km, cong={s_fast['congestion']*100:.1f}%")
    print(f"[PASS] Seoul route comfy: {len(seoul_comfy)} edges, {s_comfy['minutes']:.1f} min, {s_comfy['distance_km']:.2f} km, cong={s_comfy['congestion']*100:.1f}%")
    print(f"[PASS] Seoul route extreme: {len(seoul_extreme)} edges, {s_extreme['minutes']:.1f} min, {s_extreme['distance_km']:.2f} km, cong={s_extreme['congestion']*100:.1f}%")

    # 존재하지 않는 정류장 입력 시 크래시 없이 None 반환
    missing = bus_route_logic.find_paths_with_pathfinder(
        start_id="999999999",
        end_id="101000311",
        hour=8,
        minute=0,
        stops=stops,
    )
    assert missing["shortest_path"] is None
    assert missing["less_crowded_path"] is None
    print("[PASS] Missing stop handled cleanly")
    # 좌표 및 장소명 -> 가장 가까운 정류장 탐색 검증
    assert bus_route_logic.parse_coordinates("37.6194, 127.0598") == (37.6194, 127.0598)
    assert bus_route_logic.parse_coordinates("127.0598 37.6194") == (37.6194, 127.0598)
    assert bus_route_logic.parse_coordinates("서울역") is None

    near_kw = bus_route_logic.resolve_place_to_stop("37.6194, 127.0598", stops)
    assert near_kw is not None and near_kw["method"] == "coordinates"
    assert near_kw["distance_km"] < 1.0
    print(f"[PASS] Nearest stop to (37.6194, 127.0598): {stops[near_kw['stop_id']]['name']} ({near_kw['stop_id']}), {near_kw['distance_km']*1000:.0f}m")

    by_name = bus_route_logic.resolve_place_to_stop("광운대학교", stops)
    assert by_name is not None and "광운" in stops[by_name["stop_id"]]["name"]
    print(f"[PASS] Name/place resolution for '광운대학교': {stops[by_name['stop_id']]['name']} ({by_name['stop_id']}, method={by_name['method']})")

    # 혼잡도 20% 단위 색상 매핑(파랑/초록/노랑/주황/빨강) 검증
    expected_colors = [
        (0.05, "#2563EB"),  # 0~20% 파랑
        (0.25, "#16A34A"),  # 20~40% 초록
        (0.45, "#EAB308"),  # 40~60% 노랑
        (0.65, "#F97316"),  # 60~80% 주황
        (0.95, "#DC2626"),  # 80~100% 빨강
        (None, "#6B7280"),  # 자료 없음 회색
    ]
    for val, hex_col in expected_colors:
        info = bus_route_logic.congestion_color_info(val)
        assert info["line_color"] == hex_col, f"Mismatch for {val}: {info}"

    rows, styles = bus_route_logic.group_legs_with_styles(seoul_comfy)
    assert len(rows) == len(styles) >= 1
    assert all("bg_color" in s and "text_color" in s for s in styles)
    print(f"[PASS] 20%-tier congestion colors verified ({len(rows)} legs styled)")

    # 단절된 정류장(예: 공항버스 전용 강남역 122000606) 선택 시 인접 정류장 폴백으로 경로 탐색 검증
    fallback_res = bus_route_logic.find_paths_with_pathfinder(
        start_id="110000234",  # 광운대학교
        end_id="122000606",    # 공항버스 전용 강남역
        hour=14,
        minute=41,
        alpha=bus_route_logic.CONGESTION_WEIGHT,
        stops=stops,
    )
    assert fallback_res["shortest_path"] is not None and len(fallback_res["shortest_path"]) > 0
    assert fallback_res["less_crowded_path"] is not None and len(fallback_res["less_crowded_path"]) > 0
    assert all(edge["congestion"] is not None for edge in fallback_res["shortest_path"])
    assert all(edge["congestion"] is not None for edge in fallback_res["less_crowded_path"])
    assert fallback_res["actual_end_id"] != "122000606"
    print(f"[PASS] Nearby-stop fallback: 110000234 -> {fallback_res['actual_end_id']} ({stops[fallback_res['actual_end_id']]['name']})")

    print("ALL UI-PATHFINDER TESTS PASSED")


if __name__ == "__main__":
    main()
