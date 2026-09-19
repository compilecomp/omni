#!/usr/bin/env python3
"""CPython benchmarks for comparison with Omni Tier 0 interpreter.

Each function corresponds to an Omni bytecode module built in the C++
benchmark driver (bench/bench_omni.cpp). We measure wall-clock time
and report seconds.

Workloads (identical N for both CPython and Omni):
  1. loop_sum(N)           — sum 0..N-1 in a tight integer while-loop.
  2. sum_of_squares(N)     — sum i*i for i in 0..N-1.
  3. prop_access_loop(N)   — create object, read .x in a loop N times.
  4. range_loop_sum(N)     — sum 0..N-1 using range iterator.

Note: factorial is omitted because Omni uses int64 (no BigInt yet), so
factorial(20+) overflows. fib_recursive is omitted because building a
multi-function bytecode module by hand for the Tier 0 interpreter is
out of scope for this benchmark sweep.
"""
import sys
import time

def loop_sum(n):
    acc = 0
    i = 0
    while i < n:
        acc += i
        i += 1
    return acc

def sum_of_squares(n):
    acc = 0
    i = 0
    while i < n:
        acc += i * i
        i += 1
    return acc

class Point:
    __slots__ = ("x", "y")
    def __init__(self, x, y):
        self.x = x
        self.y = y

def prop_access_loop(n):
    p = Point(10, 20)
    acc = 0
    i = 0
    while i < n:
        acc += p.x
        i += 1
    return acc

def range_loop_sum(n):
    acc = 0
    for i in range(n):
        acc += i
    return acc

def bench(name, fn, *args, repeat=3):
    """Run fn(*args) `repeat` times; return the best wall-clock time in seconds."""
    best = float("inf")
    result = None
    for _ in range(repeat):
        t0 = time.perf_counter()
        result = fn(*args)
        t1 = time.perf_counter()
        dt = t1 - t0
        if dt < best:
            best = dt
    return best, result

def main():
    # N values: loop_sum and range_loop_sum use 10M (sum fits in int64).
    # sum_of_squares uses 2M (sum(i^2, i=0..2M-1) ~ 2.66e18, fits in int64).
    # prop_access_loop uses 5M.
    benches = [
        ("loop_sum",         loop_sum,         10_000_000),
        ("sum_of_squares",   sum_of_squares,    2_000_000),
        ("prop_access_loop", prop_access_loop,  5_000_000),
        ("range_loop_sum",   range_loop_sum,   10_000_000),
    ]
    print(f"{'workload':<22} {'N':>12} {'result':>20} {'time_s':>10}")
    print("-" * 70)
    for name, fn, n in benches:
        dt, result = bench(name, fn, n)
        print(f"{name:<22} {n:>12} {str(result):>20} {dt:>10.6f}")

if __name__ == "__main__":
    main()
