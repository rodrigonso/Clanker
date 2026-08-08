"""agentbench -- compare coding-agent CLIs on memory.

    python3 -m agentbench measure --sessions 1 --repeat 5
    python3 -m agentbench demo
    python3 -m agentbench watch
    python3 -m agentbench doctor
"""

from __future__ import annotations

import argparse
import sys

from . import demo, doctor, measure, watch
from .tools import DEFAULT_TOOLS, resolve


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="agentbench",
        description="Compare coding-agent CLIs on process-tree memory (Linux/WSL).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="known tools: " + " ".join(DEFAULT_TOOLS),
    )
    sub = parser.add_subparsers(dest="command", required=True)

    def add_tools(p):
        p.add_argument("--tools", nargs="*", default=None, metavar="NAME",
                       help="tools to include (default: all)")

    m = sub.add_parser("measure", help="run the benchmark and report PSS")
    add_tools(m)
    m.add_argument("--sessions", type=int, default=1,
                   help="concurrent sessions per tool (default: 1)")
    m.add_argument("--repeat", type=int, default=3,
                   help="repeats per tool, reported as a median (default: 3)")
    m.add_argument("--timeout", type=float, default=45.0,
                   help="seconds to wait for a session to become ready")
    m.add_argument("--settle", type=float, default=1.0,
                   help="seconds to wait after ready before sampling")
    m.add_argument("--cwd", default="/tmp/agentbench-work",
                   help="working directory the agents are launched in")
    m.add_argument("--json-out", default=None, metavar="PATH")
    m.set_defaults(func=measure.main)

    d = sub.add_parser("demo", help="side-by-side tmux panes with a live readout")
    add_tools(d)
    d.add_argument("--detached", action="store_true",
                   help="build the session but do not attach")
    d.set_defaults(func=demo.main)

    w = sub.add_parser("watch", help="live PSS readout for running agents")
    add_tools(w)
    w.add_argument("--interval", type=float, default=1.0)
    w.set_defaults(func=watch.main)

    doc = sub.add_parser("doctor", help="check tools, platform and memory")
    doc.set_defaults(func=doctor.main, tools=None)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command != "doctor":
        args.tools = resolve(args.tools)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
