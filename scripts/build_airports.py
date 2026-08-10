#!/usr/bin/env python3
"""Build the on-device runway dataset from OurAirports.

Includes large *and* medium airports: plenty of airports that dominate a local
radar picture (Phoenix-Mesa Gateway KIWA, for example) are classified
``medium_airport`` upstream, so a large-only dataset leaves them invisible.

Medium airports are gated on ``MIN_MEDIUM_RUNWAY_M`` so the dataset stays
dominated by fields that actually carry ADS-B traffic rather than every paved
strip in the world.
"""

from __future__ import annotations

import csv
import io
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_H = ROOT / "include" / "data" / "airports.h"
OUT_CPP = ROOT / "src" / "data" / "airports_data.cpp"

AIRPORTS_URL = (
    "https://raw.githubusercontent.com/davidmegginson/ourairports-data/main/"
    "airports.csv"
)
RUNWAYS_URL = (
    "https://raw.githubusercontent.com/davidmegginson/ourairports-data/main/"
    "runways.csv"
)

INCLUDED_TYPES = ("large_airport", "medium_airport")

# Every large airport is kept. A medium airport needs one runway at least this
# long to earn its ~50 bytes of flash.
MIN_MEDIUM_RUNWAY_M = 1200

# Runway designator, e.g. "12L" or "04". Four chars + NUL covers every ident in
# the upstream data.
IDENT_LEN = 5


def fetch_csv(url: str) -> list[dict[str, str]]:
    with urllib.request.urlopen(url, timeout=60) as resp:
        text = resp.read().decode("utf-8")
    return list(csv.DictReader(io.StringIO(text)))


def coord_e7(s: str | None) -> int | None:
    if not s or not s.strip():
        return None
    return int(round(float(s) * 1e7))


def is_h_designator(s: str) -> bool:
    if not s or s[0] != "H":
        return False
    rest = s[1:]
    if not rest:
        return True
    if rest[0] in "-_":
        return True
    return rest.isdigit()


def is_helipad(row: dict[str, str]) -> bool:
    le = (row.get("le_ident") or "").strip().upper()
    he = (row.get("he_ident") or "").strip().upper()
    if not is_h_designator(le) and not is_h_designator(he):
        return False
    try:
        length_ft = int(row.get("length_ft") or 0)
    except ValueError:
        length_ft = 0
    if is_h_designator(le) and is_h_designator(he):
        return True
    return length_ft < 2500


def clean_ident(s: str | None) -> str:
    """Runway designator trimmed to what the display can render."""
    return (s or "").strip().upper()[: IDENT_LEN - 1]


class Segment:
    __slots__ = (
        "airport_ident",
        "le_lat",
        "le_lon",
        "he_lat",
        "he_lon",
        "length_m",
        "le_ident",
        "he_ident",
    )

    def __init__(
        self,
        airport_ident: str,
        le_lat: int,
        le_lon: int,
        he_lat: int,
        he_lon: int,
        length_m: int,
        le_ident: str,
        he_ident: str,
    ) -> None:
        self.airport_ident = airport_ident
        self.le_lat = le_lat
        self.le_lon = le_lon
        self.he_lat = he_lat
        self.he_lon = he_lon
        self.length_m = length_m
        self.le_ident = le_ident
        self.he_ident = he_ident


def collect_segments(
    runways: list[dict[str, str]], candidates: dict[str, tuple[int, int]]
) -> dict[str, list[Segment]]:
    by_airport: dict[str, list[Segment]] = {}
    for r in runways:
        if r.get("closed") == "1":
            continue
        airport = (r.get("airport_ident") or "").strip()
        if airport not in candidates:
            continue
        if is_helipad(r):
            continue
        try:
            length_ft = int(r.get("length_ft") or 0)
        except ValueError:
            continue
        if length_ft <= 0:
            continue
        le_lat = coord_e7(r.get("le_latitude_deg"))
        le_lon = coord_e7(r.get("le_longitude_deg"))
        he_lat = coord_e7(r.get("he_latitude_deg"))
        he_lon = coord_e7(r.get("he_longitude_deg"))
        if None in (le_lat, le_lon, he_lat, he_lon):
            continue
        by_airport.setdefault(airport, []).append(
            Segment(
                airport,
                le_lat,
                le_lon,
                he_lat,
                he_lon,
                int(round(length_ft * 0.3048)),
                clean_ident(r.get("le_ident")),
                clean_ident(r.get("he_ident")),
            )
        )
    return by_airport


def build_dataset() -> tuple[
    list[tuple[str, int, int]],
    list[tuple[int, int, int, int, int, int, str, str]],
]:
    airports = fetch_csv(AIRPORTS_URL)
    runways = fetch_csv(RUNWAYS_URL)

    candidates: dict[str, tuple[int, int]] = {}
    kinds: dict[str, str] = {}
    for a in airports:
        kind = a.get("type") or ""
        if kind not in INCLUDED_TYPES:
            continue
        ident = (a.get("ident") or "").strip()
        if len(ident) != 4:
            continue
        lat = coord_e7(a.get("latitude_deg"))
        lon = coord_e7(a.get("longitude_deg"))
        if lat is None or lon is None:
            continue
        candidates[ident] = (lat, lon)
        kinds[ident] = kind

    by_airport = collect_segments(runways, candidates)

    kept: dict[str, tuple[int, int]] = {}
    for ident, coords in candidates.items():
        segments = by_airport.get(ident)
        if not segments:
            continue
        if kinds[ident] == "medium_airport" and max(
            s.length_m for s in segments
        ) < MIN_MEDIUM_RUNWAY_M:
            continue
        kept[ident] = coords

    airport_rows = sorted((ident, lat, lon) for ident, (lat, lon) in kept.items())
    airport_index = {ident: idx for idx, (ident, _, _) in enumerate(airport_rows)}

    segment_rows: list[tuple[int, int, int, int, int, int, str, str]] = []
    for ident in kept:
        for s in by_airport[ident]:
            segment_rows.append(
                (
                    airport_index[ident],
                    s.le_lat,
                    s.le_lon,
                    s.he_lat,
                    s.he_lon,
                    s.length_m,
                    s.le_ident,
                    s.he_ident,
                )
            )

    # Grouped by airport (the overlay relies on this to skip whole airports with
    # one range test) and longest-first inside each group.
    segment_rows.sort(key=lambda row: (row[0], -row[5]))
    return airport_rows, segment_rows


def render_header(airport_count: int, segment_count: int) -> str:
    return "\n".join(
        [
            "// Generated by scripts/build_airports.py — do not edit.",
            "#pragma once",
            "",
            "#include <cstddef>",
            "#include <cstdint>",
            "",
            "namespace data::airports {",
            "",
            "struct Airport {",
            "  char ident[5];",
            "  int32_t lat_e7;",
            "  int32_t lon_e7;",
            "};",
            "",
            "/** Runways are grouped by airport_idx and sorted longest-first. */",
            "struct Runway {",
            "  uint16_t airport_idx;",
            "  int32_t le_lat_e7;",
            "  int32_t le_lon_e7;",
            "  int32_t he_lat_e7;",
            "  int32_t he_lon_e7;",
            "  uint16_t length_m;",
            f"  char le_ident[{IDENT_LEN}];",
            f"  char he_ident[{IDENT_LEN}];",
            "};",
            "",
            f"constexpr size_t kAirportCount = {airport_count};",
            f"constexpr size_t kRunwayCount = {segment_count};",
            "",
            "extern const Airport kAirports[];",
            "extern const Runway kRunways[];",
            "",
            "}  // namespace data::airports",
            "",
        ]
    )


def render_cpp(
    airport_rows: list[tuple[str, int, int]],
    segments: list[tuple[int, int, int, int, int, int, str, str]],
) -> str:
    lines = [
        "// Generated by scripts/build_airports.py — do not edit.",
        '#include "data/airports.h"',
        "",
        "namespace data::airports {",
        "",
        "const Airport kAirports[] = {",
    ]
    for ident, lat, lon in airport_rows:
        lines.append(f'  {{"{ident}", {lat}, {lon}}},')
    lines += [
        "};",
        "",
        "const Runway kRunways[] = {",
    ]
    for (
        airport_idx,
        le_lat,
        le_lon,
        he_lat,
        he_lon,
        length_m,
        le_ident,
        he_ident,
    ) in segments:
        lines.append(
            f"  {{{airport_idx}, {le_lat}, {le_lon}, {he_lat}, {he_lon}, "
            f'{length_m}, "{le_ident}", "{he_ident}"}},'
        )
    lines += [
        "};",
        "",
        "}  // namespace data::airports",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    airport_rows, segments = build_dataset()
    header = render_header(len(airport_rows), len(segments))
    cpp = render_cpp(airport_rows, segments)

    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    OUT_CPP.parent.mkdir(parents=True, exist_ok=True)
    OUT_H.write_text(header, encoding="utf-8")
    OUT_CPP.write_text(cpp, encoding="utf-8")
    print(
        f"wrote {OUT_H.name} + {OUT_CPP.name} "
        f"({len(segments)} segments, {len(airport_rows)} airports)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
