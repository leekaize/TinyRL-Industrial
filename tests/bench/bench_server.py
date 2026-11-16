#!/usr/bin/env python3
"""Thin launcher for the web bench (tests/bench/).

    python3 tests/bench/bench_server.py [--nodes 2] [--steps 2400]
            [--mode two-way|node-only] [--port 8765]
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from app import main

if __name__ == "__main__":
    main()
