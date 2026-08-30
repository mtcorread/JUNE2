#!/usr/bin/env python3
"""Check that a windowed symptom policy does nothing before its start day.

Compares two runs of the same config that differ only in their symptom
policies: one with the windowed self-isolation policies, one with them removed.
The engine records no policy-relocation event, so the observable signature of a
symptom policy is the infections it prevents. Identical infection streams up to
the window's start day, differing streams from it, is the demonstration.

Producing the two runs (config_2021 starts 2020-02-01, so 12 Mar 2020 = day 40):

    ./build/disease_sim --config configs/config_2021/simulation.yaml \
        --world world_state.h5 --days 50 --runs-dir runs --run-id windowed

    # control: same config with every isolation policy deleted, leaving only
    # hospitalize_severe_cases. Keep the copy where its data/ references still
    # resolve (the loader reads them relative to the config file).
    ./build/disease_sim --config <copy>/simulation.yaml \
        --world world_state.h5 --days 50 --runs-dir runs --run-id no_policy

Usage:
    python analysis_tools/symptom_policy_window_check.py \
        runs/windowed runs/no_policy --window-start-day 40

Result on 2026-08-19: identical infection streams on days 0-39, first
difference on day 40 (15584 infections windowed vs 17101 without).
"""

import argparse
import sys
from collections import Counter
from pathlib import Path

import h5py
import numpy as np


def infections_by_day(run_dir):
    """Multiset of infection events, keyed by whole simulation day."""
    events_path = Path(run_dir) / "simulation_events.h5"
    with h5py.File(events_path, "r") as events_file:
        infections = events_file["events/infections"][:]

    by_day = {}
    for day in np.unique(np.floor(infections["time"]).astype(int)):
        on_day = infections[np.floor(infections["time"]).astype(int) == day]
        by_day[int(day)] = Counter(
            zip(
                on_day["person_id"].tolist(),
                on_day["infector_id"].tolist(),
                on_day["venue_id"].tolist(),
                on_day["time"].tolist(),
            )
        )
    return by_day


def first_differing_day(windowed, no_policy):
    for day in sorted(set(windowed) | set(no_policy)):
        if windowed.get(day, Counter()) != no_policy.get(day, Counter()):
            return day
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("windowed_run_dir")
    parser.add_argument("no_policy_run_dir")
    parser.add_argument("--window-start-day", type=int, required=True)
    arguments = parser.parse_args()

    windowed = infections_by_day(arguments.windowed_run_dir)
    no_policy = infections_by_day(arguments.no_policy_run_dir)

    last_day = max(max(windowed), max(no_policy))
    divergence_day = first_differing_day(windowed, no_policy)

    print(f"days compared:      0 - {last_day}")
    print(f"window starts:      day {arguments.window_start_day}")
    print(f"first differing day: {divergence_day}")

    for day in range(arguments.window_start_day - 2,
                     min(arguments.window_start_day + 3, last_day + 1)):
        print(f"  day {day:>3}: windowed={sum(windowed.get(day, Counter()).values()):>6}"
              f"  no_policy={sum(no_policy.get(day, Counter()).values()):>6}")

    if divergence_day is None:
        print("FAIL: the policies changed nothing at all")
        return 1
    if divergence_day < arguments.window_start_day:
        print(f"FAIL: the policies acted on day {divergence_day}, "
              f"before their window opened")
        return 1
    if divergence_day > arguments.window_start_day:
        print(f"WARN: no effect until day {divergence_day}; the window opened on "
              f"day {arguments.window_start_day}. Expected if nobody was "
              f"symptomatic and out of the house on the opening day.")
    print("PASS: no effect before the window, effect from it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
