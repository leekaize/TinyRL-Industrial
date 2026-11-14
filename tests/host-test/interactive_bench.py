#!/usr/bin/env python3
"""Thin launcher: the web bench lives in tests/host-test/webbench/."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "webbench"))
from app import main

if __name__ == "__main__":
    main()
