#!/usr/bin/env python3
"""Minimal FastLOB consumer demo."""

from __future__ import annotations

import sys
import time
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
for candidate in (_ROOT / "build", _ROOT / "build" / "python", _ROOT):
    sys.path.insert(0, str(candidate))

import pyfastlob as fl  # noqa: E402


def main() -> None:
    book = fl.OrderBook(1 << 16)
    px_bid = fl.dollars_to_ticks(100.0)
    book.add(1, fl.SIDE_BUY, px_bid, 1000)
    book.add(2, fl.SIDE_SELL, fl.dollars_to_ticks(100.10), 100)
    book.add(3, fl.SIDE_SELL, fl.dollars_to_ticks(100.20), 200)

    sim = fl.ExecutionSimulator(book)
    sim.submit_limit(9001, fl.SIDE_BUY, px_bid, 100)
    book.execute(1, 400)
    sim.on_public_trade(fl.SIDE_BUY, px_bid, 400)
    book.execute(1, 600)
    sim.on_public_trade(fl.SIDE_BUY, px_bid, 700)
    print(f"shadow fill_count={sim.fill_count()}")

    mkt = sim.submit_market(8001, fl.SIDE_BUY, 250)
    print(f"market fill: {mkt}  # price ticks; 10016 => $100.16 VWAP")

    t0 = time.perf_counter_ns()
    for _ in range(10_000):
        _ = book.snapshot(10)
    t1 = time.perf_counter_ns()
    snap = book.snapshot(5)
    print("snapshot rows [bid_px, bid_qty, ask_px, ask_qty]:")
    print(snap.to_list())
    print(f"snapshot export avg ns: {(t1 - t0) / 10_000:.1f}")
    print("buffer protocol available:", memoryview(snap).shape)


if __name__ == "__main__":
    main()
