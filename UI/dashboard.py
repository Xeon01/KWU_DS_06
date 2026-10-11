import zipfile
import folium
import pandas as pd
import streamlit as st
from streamlit_folium import st_folium
from bus_route_logic import (
    build_graph,
    crowded_stops,
    find_path,
    group_legs,
    load_data,
    path_summary,
)

def stop_label(stop_id, stops):
    stop = stops[stop_id]
    number = stop["ars_id"] or stop_id
    return f"{stop['name']} · {number}"

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
        st.markdown(f"### {title}")
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

    points = [
        point
        for index, point in enumerate(coordinates)
        if index == 0 or point != coordinates[index - 1]
    ]
    folium.PolyLine(
        points,
        color=color,
        weight=7,
        opacity=0.9,
        tooltip=label,
    ).add_to(bus_map)

    for index, row in enumerate(stop_rows, start=1):
        stop = stops.get(row["stop_id"])
        if not stop:
            continue

        is_start = row["stop_id"] == start_id
        is_end = row["stop_id"] == end_id
        marker_color = "#FCA311" if is_start else "#14213D" if is_end else color
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

def render_route_details(path, stops):
    stop_rows = route_stop_rows(path, stops)
    st.markdown(f"#### 지도 정류장 마커 ({len(stop_rows)}곳)")
    st.caption("지도에서 원형 마커를 누르면 정류장 순서, 이름, 이용 노선을 볼 수 있습니다.")

    st.markdown("#### 버스 노선별 구간 상세")
    leg_rows = group_legs(path)
    st.table(pd.DataFrame(leg_rows))

def render_search_results(result, stops):
    st.markdown("## 경로 선택")
    st.caption(
        f"{stops[result['start_id']]['name']} → {stops[result['end_id']]['name']} · "
        f"{result['hour']:02d}:00 출발 기준"
    )

    route_items = []
    possible_routes = [
        (
            "거리 우선 경로",
            result["shortest_path"],
            "거리 우선",
            "정류장 좌표 사이의 직선거리를 기준으로 찾은 경로",
        ),
        (
            "혼잡 회피 경로",
            result["less_crowded_path"],
            "혼잡 회피",
            "이동 시간에 혼잡도를 반영해 찾은 경로",
        ),
    ]
    for title, path, badge, description in possible_routes:
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
    route_choices = {}
    for route in route_items:
        congestion = route["congestion"]
        if congestion_is_different and congestion is not None:
            is_less_crowded = congestion == min(known_congestion)
            route_color = "#16A34A" if is_less_crowded else "#DC2626"
            traffic_label = "🟢 혼잡도 낮음" if is_less_crowded else "🔴 혼잡도 높음"
        elif len(route_items) > 1:
            route_color = "#FCA311"
            traffic_label = (
                "🟠 혼잡도 비슷함"
                if len(known_congestion) == len(route_items)
                else "🟠 혼잡도 비교 자료 부족"
            )
        else:
            route_color = "#FCA311"
            traffic_label = "🟠 비교할 다른 경로가 없습니다"

        option_label = (
            f"{traffic_label} · {route['title']} · {route['congestion_text']}"
        )
        route["color"] = route_color
        route_choices[option_label] = route

    selected_label = st.radio(
        "지도에 표시할 경로를 선택하세요",
        list(route_choices),
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
        st.markdown(f"### 노선 지도 · {selected_route['title']}")
        with st.container(border=True):
            render_map(
                selected_route["path"],
                stops,
                result["start_id"],
                result["end_id"],
                selected_route["color"],
                selected_route["title"],
            )
            st.caption("지도에는 선택한 경로만 표시합니다.")

    with detail_col:
        st.markdown("### 경로 상세")
        with st.container(border=True):
            render_route_details(selected_route["path"], stops)

def render_crowded_stops(hour, stops):
    st.markdown("### 시간대별 혼잡 정류장 TOP 10")
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


def main():
    st.set_page_config(page_title="버스 경로 추천", page_icon="🚌", layout="wide")
    st.title("🚌 버스 경로 추천", text_alignment="center")

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

    with st.container(border=True):
        st.markdown("### 여정 입력")
        st.caption("정류장 이름이나 번호로 검색하고 출발 시간대를 선택하세요.")
        with st.form("route_search"):
            origin_col, destination_col, time_col = st.columns([2, 2, 1], gap="medium")
            with origin_col:
                st.markdown("🟡 **출발 정류장** · 타는 곳")
                start_id = st.selectbox(
                    "출발 정류장",
                    station_ids,
                    index=None,
                    placeholder="출발 정류장 검색",
                    format_func=lambda stop_id: stop_label(stop_id, stops),
                    label_visibility="collapsed",
                    key="origin_stop",
                )
            with destination_col:
                st.markdown("🔵 **도착 정류장** · 내리는 곳")
                end_id = st.selectbox(
                    "도착 정류장",
                    station_ids,
                    index=None,
                    placeholder="도착 정류장 검색",
                    format_func=lambda stop_id: stop_label(stop_id, stops),
                    label_visibility="collapsed",
                    key="destination_stop",
                )
            with time_col:
                hour = st.selectbox(
                    "출발 시간대",
                    range(24),
                    format_func=lambda value: f"{value:02d}:00",
                )
            search_clicked = st.form_submit_button(
                "경로 검색",
                type="primary",
                use_container_width=True,
            )

    if search_clicked:
        st.session_state.pop("search_result", None)
        if not start_id or not end_id:
            st.warning("출발 정류장과 도착 정류장을 모두 선택하세요.")
        elif start_id == end_id:
            st.warning("출발 정류장과 도착 정류장을 다르게 선택하세요.")
        else:
            graph = build_graph(hour)
            st.session_state["search_result"] = {
                "hour": hour,
                "start_id": start_id,
                "end_id": end_id,
                "shortest_path": find_path(graph, start_id, end_id),
                "less_crowded_path": find_path(
                    graph,
                    start_id,
                    end_id,
                    avoid_congestion=True,
                ),
            }

    result = st.session_state.get("search_result")
    if result:
        render_search_results(result, stops)
    else:
        st.info("출발지와 도착지를 선택한 다음 경로를 검색하세요.")

    crowded_hour = result["hour"] if result else hour
    render_crowded_stops(crowded_hour, stops)

    st.caption("거리는 정류장 좌표 사이의 직선거리 추정값이며, 실제 도로 거리는 다를 수 있습니다.")


if __name__ == "__main__":
    main()
