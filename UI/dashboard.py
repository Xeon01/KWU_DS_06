import zipfile
from datetime import datetime
import folium
import pandas as pd
import streamlit as st
import streamlit.components.v1 as components
from streamlit_folium import st_folium
from bus_route_logic import (
    CONGESTION_COLOR_BANDS,
    CONGESTION_WEIGHT,
    build_graph,
    congestion_color_info,
    crowded_stops,
    filter_reachable_stop_candidates,
    find_paths_with_pathfinder,
    group_legs_with_styles,
    load_data,
    path_summary,
    resolve_place_to_nearest_stops,
)

def stop_label(stop_id, stops):
    stop = stops[stop_id]
    number = stop["ars_id"] or stop_id
    return f"{stop['name']} · {number}"


def format_distance_text(distance_km):
    if distance_km is None:
        return ""
    meters = round(distance_km * 1000)
    if meters < 1000:
        return f"{meters}m"
    return f"{distance_km:.2f}km"


def stop_option_label(stop_id, stops, distance_map=None):
    base = stop_label(stop_id, stops)
    if distance_map and stop_id in distance_map:
        dist_text = format_distance_text(distance_map[stop_id])
        if dist_text:
            label_dist = "0m" if dist_text == "0m" else f"약 {dist_text}"
            return f"{base} ({label_dist})"
    return base

def route_stop_rows(path, stops):
    stop_ids = [path[0]["start_id"]] + [edge["end_id"] for edge in path]
    rows = []

    for index, stop_id in enumerate(stop_ids):
        if index == 0:
            kind = "출발"
            route_name = path[0]["route_name"]
        elif index == len(stop_ids) - 1:
            kind = "도착"
            route_name = path[-1]["route_name"]
        else:
            previous_route = path[index - 1]["route_id"]
            next_route = path[index]["route_id"]
            kind = "환승" if previous_route != next_route else "경유"
            route_name = path[index]["route_name"]

        stop = stops.get(stop_id, {})
        rows.append(
            {
                "stop_id": stop_id,
                "kind": kind,
                "name": stop.get("name", stop_id),
                "number": stop.get("ars_id", "") or stop_id,
                "route_name": route_name,
            }
        )
    return rows

def render_route_card(title, description, path, badge):
    with st.container(border=True):
        st.markdown(f"**{badge}**")
        st.subheader(title, anchor=False)
        st.caption(description)

        summary = path_summary(path)
        distance_col, time_col, transfer_col = st.columns(3)
        distance_col.metric("거리 추정", f"{summary['distance_km']:.1f} km")
        time_col.metric("예상 시간", f"{summary['minutes']:.0f} 분")
        transfer_col.metric("환승", f"{summary['transfers']} 회")

        if summary["congestion"] is None:
            st.caption("혼잡도 자료 없음")
        else:
            percent = summary["congestion"] * 100
            st.progress(
                min(max(summary["congestion"], 0.0), 1.0),
                text=f"평균 혼잡도 {percent:.1f}%",
            )


def render_map(path, stops, start_id, end_id, color, label):
    stop_rows = route_stop_rows(path, stops)
    coordinates = [
        [stops[row["stop_id"]]["latitude"], stops[row["stop_id"]]["longitude"]]
        for row in stop_rows
        if row["stop_id"] in stops
    ]

    if not coordinates:
        st.info("지도에 표시할 정류장 좌표가 없습니다.")
        return

    center = [
        sum(point[0] for point in coordinates) / len(coordinates),
        sum(point[1] for point in coordinates) / len(coordinates),
    ]
    bus_map = folium.Map(location=center, zoom_start=12, control_scale=True)

    for edge in path:
        s_stop = stops.get(edge["start_id"])
        e_stop = stops.get(edge["end_id"])
        if not s_stop or not e_stop:
            continue
        seg_points = [
            [s_stop["latitude"], s_stop["longitude"]],
            [e_stop["latitude"], e_stop["longitude"]],
        ]
        if seg_points[0] == seg_points[1]:
            continue
        seg_info = congestion_color_info(edge["congestion"])
        cong_text = (
            f"{edge['congestion'] * 100:.1f}%"
            if edge["congestion"] is not None
            else "자료 없음"
        )
        folium.PolyLine(
            seg_points,
            color=seg_info["line_color"],
            weight=7,
            opacity=0.9,
            tooltip=f"{edge['route_name']} ({s_stop['name']} → {e_stop['name']}) · 혼잡도 {cong_text}",
        ).add_to(bus_map)

    for index, row in enumerate(stop_rows, start=1):
        stop = stops.get(row["stop_id"])
        if not stop:
            continue

        is_start = row["stop_id"] == start_id
        is_end = row["stop_id"] == end_id
        edge_idx = min(index - 1, len(path) - 1)
        seg_color = congestion_color_info(path[edge_idx]["congestion"])["line_color"]
        marker_color = "#FCA311" if is_start else "#14213D" if is_end else seg_color
        popup_html = (
            f"<b>{row['kind']} · {index}번 정류장</b><br>"
            f"{row['name']} ({row['number']})<br>이용 노선: {row['route_name']}"
        )
        if is_start or is_end:
            marker_text = "출발" if is_start else "도착"
            marker_text_color = "#14213D" if is_start else "#FFFFFF"
            folium.Marker(
                [stop["latitude"], stop["longitude"]],
                tooltip=f"{marker_text}: {row['name']}",
                popup=folium.Popup(popup_html, max_width=280),
                icon=folium.DivIcon(
                    html=(
                        f'<div style="width:48px;height:48px;display:flex;'
                        f'align-items:center;justify-content:center;background:{marker_color};'
                        f'color:{marker_text_color};border:3px solid #FFFFFF;border-radius:50%;'
                        'font-size:13px;font-weight:800;white-space:nowrap;'
                        'box-shadow:0 2px 7px #555;">'
                        f"{marker_text}</div>"
                    ),
                    icon_size=(48, 48),
                    icon_anchor=(24, 24),
                ),
            ).add_to(bus_map)
        else:
            folium.CircleMarker(
                [stop["latitude"], stop["longitude"]],
                radius=7,
                color="#FFFFFF",
                weight=2,
                fill=True,
                fill_color=marker_color,
                fill_opacity=1,
                tooltip=f"{index}. {row['kind']} · {row['name']}",
                popup=folium.Popup(popup_html, max_width=280),
            ).add_to(bus_map)

    st_folium(bus_map, height=500, use_container_width=True, returned_objects=[])

def render_congestion_legend():
    items = [
        (label, line_color)
        for _, label, line_color, _, _ in CONGESTION_COLOR_BANDS
    ] + [("자료 없음", "#6B7280")]
    badges = "".join(
        f'<span style="display:inline-flex;align-items:center;gap:6px;margin-right:12px;font-size:12px;">'
        f'<span style="width:14px;height:6px;border-radius:3px;background:{hex_color};display:inline-block;"></span>'
        f'{label}</span>'
        for label, hex_color in items
    )
    st.markdown(f'<div style="margin-top:4px;margin-bottom:4px;">{badges}</div>', unsafe_allow_html=True)


def render_route_details(path, stops):
    stop_rows = route_stop_rows(path, stops)
    st.markdown(f"**지도 정류장 마커 ({len(stop_rows)}곳)**")
    st.caption("지도에서 원형 마커를 누르면 정류장 순서, 이름, 이용 노선을 볼 수 있습니다.")

    st.markdown("**버스 노선별 구간 상세**")
    leg_rows, leg_styles = group_legs_with_styles(path)
    table = pd.DataFrame(leg_rows)
    try:
        def _style_row(row):
            info = leg_styles[row.name]
            css = f"background-color: {info['bg_color']}; color: {info['text_color']};"
            return [css] * len(row)

        st.table(table.style.apply(_style_row, axis=1))
    except Exception:
        st.table(table)

def render_search_results(result, stops):
    st.header("경로 선택", anchor=False)
    st.caption(
        f"{stops[result['start_id']]['name']} → {stops[result['end_id']]['name']} · "
        f"{result['hour']:02d}:{result.get('minute', 0):02d} 출발 기준"
    )
    if result.get("fallback_notice"):
        st.info(result["fallback_notice"])

    route_items = []
    possible_routes = [
        (
            "혼잡 회피 경로",
            result["less_crowded_path"],
            "혼잡 회피",
            f"이동 시간과 혼잡도를 함께 반영한 경로",
            False,
        ),
        (
            "최단 시간 경로",
            result["shortest_path"],
            "최단 시간",
            "시간대 전환을 반영한 최단 소요시간 경로",
            False,
        ),
        (
            "극단적 회피 경로",
            result.get("extreme_path"),
            "극단적 회피",
            "혼잡도×시간 비용만 최소화한 경로 (소요시간이 크게 늘어날 수 있음)",
            True,
        ),
    ]
    for title, path, badge, description, is_extreme in possible_routes:
        if path is None:
            continue
        summary = path_summary(path)
        congestion_text = (
            f"혼잡도 {summary['congestion'] * 100:.1f}%"
            if summary["congestion"] is not None
            else "혼잡도 자료 없음"
        )
        route_items.append({
            "title": title,
            "path": path,
            "badge": badge,
            "description": description,
            "is_extreme": is_extreme,
            "minutes": summary["minutes"],
            "congestion": summary["congestion"],
            "congestion_text": congestion_text,
        })

    if not route_items:
        st.error("출발지에서 도착지까지 갈 수 있는 경로를 찾지 못했습니다.")
        return

    known_congestion = [
        route["congestion"] for route in route_items
        if route["congestion"] is not None
    ]
    congestion_is_different = (
        len(known_congestion) >= 2
        and max(known_congestion) - min(known_congestion) > 0.000001
    )
    min_congestion = min(known_congestion) if known_congestion else None
    max_congestion = max(known_congestion) if known_congestion else None
    route_choices = {}
    for route in route_items:
        congestion = route["congestion"]
        if congestion_is_different and congestion is not None:
            if abs(congestion - min_congestion) <= 1e-6:
                route_color = "#16A34A"
                traffic_label = "혼잡도 낮음"
            elif abs(congestion - max_congestion) <= 1e-6:
                route_color = "#DC2626"
                traffic_label = "혼잡도 높음"
            else:
                route_color = "#FCA311"
                traffic_label = "혼잡도 중간"
        elif len(route_items) > 1:
            route_color = "#FCA311"
            traffic_label = (
                "혼잡도 비슷함"
                if len(known_congestion) == len(route_items)
                else "혼잡도 비교 자료 부족"
            )
        else:
            route_color = "#FCA311"
            traffic_label = "비교할 다른 경로가 없습니다"

        option_label = (
            f"{traffic_label} · {route['title']} · {route['minutes']:.0f}분 · {route['congestion_text']}"
        )
        route["color"] = route_color
        route_choices[option_label] = route

    route_options = list(route_choices)
    default_index = 0
    best_congestion = None
    standard_indices = [
        idx for idx, label in enumerate(route_options)
        if not route_choices[label]["is_extreme"]
    ]
    for idx in (standard_indices or range(len(route_options))):
        cong = route_choices[route_options[idx]]["congestion"]
        if cong is not None and (best_congestion is None or cong < best_congestion):
            best_congestion = cong
            default_index = idx

    selected_label = st.radio(
        "지도에 표시할 경로를 선택하세요",
        route_options,
        index=default_index,
        horizontal=True,
    )
    selected_route = route_choices[selected_label]

    render_route_card(
        selected_route["title"],
        selected_route["description"],
        selected_route["path"],
        selected_route["badge"],
    )

    map_col, detail_col = st.columns([1.5, 1], gap="large")
    with map_col:
        st.subheader(f"노선 지도 · {selected_route['title']}", anchor=False)
        with st.container(border=True):
            render_map(
                selected_route["path"],
                stops,
                result["start_id"],
                result["end_id"],
                selected_route["color"],
                selected_route["title"],
            )
            render_congestion_legend()
            st.caption("구간별 혼잡도 표시")

    with detail_col:
        st.subheader("경로 상세", anchor=False)
        with st.container(border=True):
            render_route_details(selected_route["path"], stops)

def render_crowded_stops(hour, stops):
    st.subheader("시간대별 혼잡 정류장 TOP 10", anchor=False)
    st.caption(f"{hour:02d}:00 기준으로 연결 구간의 평균 혼잡도를 계산합니다.")
    graph = build_graph(hour)
    rows = crowded_stops(graph, stops)
    if not rows:
        st.info("선택한 시간대에 혼잡도 데이터가 없습니다.")
        return

    table = pd.DataFrame(rows)
    table["평균 혼잡도"] = (table["평균 혼잡도"] * 100).map(
        lambda value: f"{value:.1f}%"
    )
    st.table(table)


def scroll_to_results(trigger_seq):
    """여정 입력 완료 후 검색 결과('경로 선택') 위치로 부드럽게 스크롤한다."""
    components.html(
        f"""
        <script>
        (function() {{
            // scroll trigger #{trigger_seq}
            try {{
                const doc = window.parent.document;
                const doScroll = () => {{
                    const headings = doc.querySelectorAll("h2");
                    const target = Array.from(headings).find(
                        (el) => el.textContent && el.textContent.trim().includes("경로 선택")
                    );
                    if (target) {{
                        target.style.scrollMarginTop = "4rem";
                        target.scrollIntoView({{ behavior: "smooth", block: "start" }});
                    }}
                }};
                window.parent.requestAnimationFrame(() => {{
                    setTimeout(doScroll, 120);
                }});
            }} catch (e) {{
                // 부모 DOM 접근이 제한된 환경에서는 무시
            }}
        }})();
        </script>
        """,
        height=0,
    )


def main():
    st.set_page_config(page_title="버스 경로 추천", layout="wide")
    st.markdown(
        "<div style='text-align: center; font-size: 2.25rem; font-weight: 700; line-height: 1.2; margin-bottom: 0.75rem;'>버스 경로 추천</div>",
        unsafe_allow_html=True,
    )

    try:
        edges, stops = load_data()
    except (FileNotFoundError, OSError, zipfile.BadZipFile, StopIteration, KeyError, ValueError) as error:
        st.error(f"데이터 파일을 읽지 못했습니다: {error}")
        st.info("노선 CSV와 정류장 ZIP 파일이 프로젝트 폴더에 있는지 확인하세요.")
        return

    network_stop_ids = set(edges["출발_정류장_ID"]) | set(edges["도착_정류장_ID"])
    station_ids = sorted(stop_id for stop_id in network_stop_ids if stop_id in stops)
    if not station_ids:
        st.error("노선 데이터와 정류장 정보가 연결되지 않았습니다.")
        return

    init_time = st.session_state.setdefault("init_departure_time", datetime.now())
    st.session_state.setdefault("departure_hour", init_time.hour)
    st.session_state.setdefault("departure_minute", init_time.minute)

    with st.container(border=True):
        st.subheader("여정 입력", anchor=False)
        st.caption(
            "출발 시각과 장소명·주소(또는 위도,경도)를 입력하면 가장 가까운 정류장 5곳 중 "
            "연결 가능한 정류장만 거리순 드롭다운으로 제공합니다."
        )

        time_col, minute_col, _spacer_col = st.columns([1, 1, 4], gap="medium")
        with time_col:
            hour = st.selectbox(
                "출발 시",
                range(24),
                key="departure_hour",
                format_func=lambda value: f"{value:02d}시",
            )
        with minute_col:
            minute = st.selectbox(
                "출발 분",
                range(60),
                key="departure_minute",
                format_func=lambda value: f"{value:02d}분",
            )

        with st.form("place_lookup"):
            place_origin_col, place_dest_col = st.columns(2, gap="medium")
            with place_origin_col:
                origin_place = st.text_input(
                    "출발지 검색 (장소명·주소 또는 위도,경도)",
                    placeholder="예: 광운대학교 또는 37.6194, 127.0598",
                )
            with place_dest_col:
                dest_place = st.text_input(
                    "도착지 검색 (장소명·주소 또는 위도,경도)",
                    placeholder="예: 서울역 또는 37.5547, 126.9707",
                )
            lookup_clicked = st.form_submit_button(
                "가까운 정류장 찾기",
                use_container_width=True,
            )

        if lookup_clicked:
            notices = []
            origin_query = origin_place.strip()
            dest_query = dest_place.strip()

            if not origin_query and not dest_query:
                notices.append(("warning", "출발지 또는 도착지를 입력하세요."))
            else:
                origin_raw = st.session_state.get("origin_raw_candidates")
                dest_raw = st.session_state.get("dest_raw_candidates")

                if origin_query:
                    origin_raw = resolve_place_to_nearest_stops(
                        origin_query, stops, station_ids, limit=5
                    )
                    if origin_raw:
                        st.session_state["origin_raw_candidates"] = origin_raw
                    else:
                        st.session_state.pop("origin_raw_candidates", None)
                        st.session_state.pop("origin_dropdown_candidates", None)
                        notices.append(
                            ("warning", f"출발지 '{origin_query}'에 가까운 정류장을 찾지 못했습니다.")
                        )
                        origin_raw = None

                if dest_query:
                    dest_raw = resolve_place_to_nearest_stops(
                        dest_query, stops, station_ids, limit=5
                    )
                    if dest_raw:
                        st.session_state["dest_raw_candidates"] = dest_raw
                    else:
                        st.session_state.pop("dest_raw_candidates", None)
                        st.session_state.pop("dest_dropdown_candidates", None)
                        notices.append(
                            ("warning", f"도착지 '{dest_query}'에 가까운 정류장을 찾지 못했습니다.")
                        )
                        dest_raw = None

                try:
                    if origin_raw and dest_raw:
                        filtered = filter_reachable_stop_candidates(
                            origin_raw, dest_raw, hour=hour, minute=minute
                        )
                        st.session_state["origin_dropdown_candidates"] = filtered["origin_options"]
                        st.session_state["dest_dropdown_candidates"] = filtered["dest_options"]
                        st.session_state["origin_stop"] = filtered["default_origin_id"]
                        st.session_state["destination_stop"] = filtered["default_dest_id"]

                        if filtered["origin_filtered"]:
                            notices.append((
                                "success",
                                f"출발지: 가까운 정류장 {len(origin_raw)}곳 중 연결 가능한 "
                                f"{len(filtered['origin_options'])}곳을 거리순으로 표시합니다.",
                            ))
                        else:
                            notices.append((
                                "warning",
                                f"출발지 주변 정류장 {len(origin_raw)}곳에서 도착지 후보까지 "
                                f"연결되는 경로를 찾지 못해 가까운 {len(origin_raw)}곳을 모두 거리순으로 표시합니다.",
                            ))

                        if filtered["dest_filtered"]:
                            notices.append((
                                "success",
                                f"도착지: 가까운 정류장 {len(dest_raw)}곳 중 연결 가능한 "
                                f"{len(filtered['dest_options'])}곳을 거리순으로 표시합니다.",
                            ))
                        else:
                            notices.append((
                                "warning",
                                f"도착지 주변 정류장 {len(dest_raw)}곳으로 연결되는 경로를 찾지 못해 "
                                f"가까운 {len(dest_raw)}곳을 모두 거리순으로 표시합니다.",
                            ))
                    elif origin_raw:
                        existing_dest = st.session_state.get("destination_stop")
                        if existing_dest:
                            filtered = filter_reachable_stop_candidates(
                                origin_raw,
                                [{"stop_id": existing_dest, "distance_km": 0.0}],
                                hour=hour,
                                minute=minute,
                            )
                            st.session_state["origin_dropdown_candidates"] = filtered["origin_options"]
                            st.session_state["origin_stop"] = filtered["default_origin_id"]
                            if filtered["origin_filtered"]:
                                notices.append((
                                    "success",
                                    f"출발지: 가까운 정류장 {len(origin_raw)}곳 중 선택한 도착지까지 "
                                    f"연결 가능한 {len(filtered['origin_options'])}곳을 거리순으로 표시합니다.",
                                ))
                            else:
                                notices.append((
                                    "warning",
                                    f"출발지 주변 정류장 {len(origin_raw)}곳에서 선택한 도착지까지 "
                                    f"연결되는 경로를 찾지 못해 가까운 {len(origin_raw)}곳을 모두 거리순으로 표시합니다.",
                                ))
                        else:
                            st.session_state["origin_dropdown_candidates"] = origin_raw
                            st.session_state["origin_stop"] = origin_raw[0]["stop_id"]
                            notices.append((
                                "info",
                                f"출발지 주변 가까운 정류장 {len(origin_raw)}곳을 거리순으로 표시했습니다. "
                                "도착지도 검색하면 연결 가능한 정류장만 필터링됩니다.",
                            ))
                    elif dest_raw:
                        existing_origin = st.session_state.get("origin_stop")
                        if existing_origin:
                            filtered = filter_reachable_stop_candidates(
                                [{"stop_id": existing_origin, "distance_km": 0.0}],
                                dest_raw,
                                hour=hour,
                                minute=minute,
                            )
                            st.session_state["dest_dropdown_candidates"] = filtered["dest_options"]
                            st.session_state["destination_stop"] = filtered["default_dest_id"]
                            if filtered["dest_filtered"]:
                                notices.append((
                                    "success",
                                    f"도착지: 가까운 정류장 {len(dest_raw)}곳 중 선택한 출발지에서 "
                                    f"연결 가능한 {len(filtered['dest_options'])}곳을 거리순으로 표시합니다.",
                                ))
                            else:
                                notices.append((
                                    "warning",
                                    f"도착지 주변 정류장 {len(dest_raw)}곳으로 연결되는 경로를 찾지 못해 "
                                    f"가까운 {len(dest_raw)}곳을 모두 거리순으로 표시합니다.",
                                ))
                        else:
                            st.session_state["dest_dropdown_candidates"] = dest_raw
                            st.session_state["destination_stop"] = dest_raw[0]["stop_id"]
                            notices.append((
                                "info",
                                f"도착지 주변 가까운 정류장 {len(dest_raw)}곳을 거리순으로 표시했습니다. "
                                "출발지도 검색하면 연결 가능한 정류장만 필터링됩니다.",
                            ))
                except RuntimeError as error:
                    notices.append(("error", f"정류장 연결성 검사 중 오류가 발생했습니다: {error}"))

            st.session_state["lookup_notices"] = notices

        for level, msg in st.session_state.get("lookup_notices", []):
            getattr(st, level)(msg)

        origin_cands = st.session_state.get("origin_dropdown_candidates")
        dest_cands = st.session_state.get("dest_dropdown_candidates")
        if origin_cands or dest_cands:
            if st.button("전체 정류장 선택으로 복원"):
                for key in (
                    "origin_raw_candidates",
                    "dest_raw_candidates",
                    "origin_dropdown_candidates",
                    "dest_dropdown_candidates",
                    "lookup_notices",
                ):
                    st.session_state.pop(key, None)
                st.session_state["origin_stop"] = None
                st.session_state["destination_stop"] = None
                st.rerun()

        if origin_cands:
            origin_option_ids = [c["stop_id"] for c in origin_cands]
            origin_dist_map = {c["stop_id"]: c.get("distance_km") for c in origin_cands}
            if st.session_state.get("origin_stop") not in origin_option_ids:
                st.session_state["origin_stop"] = origin_option_ids[0]
        else:
            origin_option_ids = station_ids
            origin_dist_map = None

        if dest_cands:
            dest_option_ids = [c["stop_id"] for c in dest_cands]
            dest_dist_map = {c["stop_id"]: c.get("distance_km") for c in dest_cands}
            if st.session_state.get("destination_stop") not in dest_option_ids:
                st.session_state["destination_stop"] = dest_option_ids[0]
        else:
            dest_option_ids = station_ids
            dest_dist_map = None

        with st.form("route_search"):
            origin_col, destination_col = st.columns(2, gap="medium")
            with origin_col:
                st.markdown("**출발 정류장** · 타는 곳")
                start_id = st.selectbox(
                    "출발 정류장",
                    origin_option_ids,
                    index=None,
                    placeholder="출발 정류장 검색",
                    format_func=lambda stop_id: stop_option_label(stop_id, stops, origin_dist_map),
                    label_visibility="collapsed",
                    key="origin_stop",
                )
            with destination_col:
                st.markdown("**도착 정류장** · 내리는 곳")
                end_id = st.selectbox(
                    "도착 정류장",
                    dest_option_ids,
                    index=None,
                    placeholder="도착 정류장 검색",
                    format_func=lambda stop_label_id: stop_option_label(stop_label_id, stops, dest_dist_map),
                    label_visibility="collapsed",
                    key="destination_stop",
                )
            search_clicked = st.form_submit_button(
                "경로 검색",
                type="primary",
                use_container_width=True,
            )

    should_scroll_to_results = False
    if search_clicked:
        st.session_state.pop("search_result", None)
        if not start_id or not end_id:
            st.warning("출발 정류장과 도착 정류장을 모두 선택하세요.")
        elif start_id == end_id:
            st.warning("출발 정류장과 도착 정류장을 다르게 선택하세요.")
        else:
            try:
                paths = find_paths_with_pathfinder(
                    start_id=start_id,
                    end_id=end_id,
                    hour=hour,
                    minute=minute,
                    alpha=CONGESTION_WEIGHT,
                    stops=stops,
                    candidate_ids=station_ids,
                )
                actual_start_id = paths.get("actual_start_id", start_id)
                actual_end_id = paths.get("actual_end_id", end_id)
                fallback_parts = []
                if (paths["shortest_path"] or paths["less_crowded_path"] or paths.get("extreme_path")):
                    if actual_start_id != start_id:
                        fallback_parts.append(f"출발: {stop_label(actual_start_id, stops)}")
                    if actual_end_id != end_id:
                        fallback_parts.append(f"도착: {stop_label(actual_end_id, stops)}")
                fallback_notice = (
                    "선택한 정류장 간 직결 노선이 없어 인접 정류장으로 경로를 탐색했습니다 ("
                    + ", ".join(fallback_parts)
                    + ")."
                    if fallback_parts
                    else None
                )
                st.session_state["search_result"] = {
                    "hour": hour,
                    "minute": minute,
                    "alpha": CONGESTION_WEIGHT,
                    "start_id": actual_start_id,
                    "end_id": actual_end_id,
                    "shortest_path": paths["shortest_path"],
                    "less_crowded_path": paths["less_crowded_path"],
                    "extreme_path": paths.get("extreme_path"),
                    "fallback_notice": fallback_notice,
                }
                st.session_state["scroll_to_results_seq"] = (
                    st.session_state.get("scroll_to_results_seq", 0) + 1
                )
                should_scroll_to_results = True
            except RuntimeError as error:
                st.error(f"Pathfinder 실행 중 오류가 발생했습니다: {error}")

    result = st.session_state.get("search_result")
    if result:
        render_search_results(result, stops)
    else:
        st.info("출발지와 도착지를 선택한 다음 경로를 검색하세요.")

    crowded_hour = result["hour"] if result else hour
    render_crowded_stops(crowded_hour, stops)

    st.caption("거리는 정류장 좌표 사이의 직선거리 추정값이며, 실제 도로 거리는 다를 수 있습니다.")

    if should_scroll_to_results:
        scroll_to_results(st.session_state["scroll_to_results_seq"])


if __name__ == "__main__":
    main()
