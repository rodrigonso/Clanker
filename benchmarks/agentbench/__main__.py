"""agentbench -- compare coding-agent CLIs on memory.

    python3 -m agentbench measure --sessions 1 --repeat 5
    python3 -m agentbench demo
    python3 -m agentbench watch
    python3 -m agentbench doctor
"""

from __future__ import annotations

import argparse
import sys

from .tools import DEFAULT_TOOLS, LIVE_TOOLS, resolve


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="agentbench",
        description="Compare coding-agent CLIs on process-tree RSS (macOS).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="known tools: " + " ".join(DEFAULT_TOOLS),
    )
    sub = parser.add_subparsers(dest="command", required=True)

    def add_tools(p, default=None):
        p.add_argument("--tools", nargs="*", default=default, metavar="NAME",
                       help="tools to include (default: all)")

    m = sub.add_parser("measure", help="run the benchmark and report RSS")
    add_tools(m)
    m.add_argument("--sessions", type=int, default=1,
                   help="concurrent sessions per tool (default: 1)")
    m.add_argument("--repeat", type=int, default=3,
                   help="repeats per tool, reported as a median (default: 3)")
    m.add_argument("--timeout", type=float, default=45.0,
                   help="seconds to wait for a session to become ready")
    m.add_argument("--settle", type=float, default=1.0,
                   help="seconds to wait after ready before sampling")
    m.add_argument("--cwd", default="out/agentbench-work",
                   help="working directory the agents are launched in")
    m.add_argument("--json-out", default=None, metavar="PATH")

    d = sub.add_parser("demo", help="side-by-side tmux panes with a live readout")
    add_tools(d, default=LIVE_TOOLS)
    d.add_argument("--detached", action="store_true",
                   help="build the session but do not attach")

    w = sub.add_parser("watch", help="live RSS readout for running agents")
    add_tools(w, default=LIVE_TOOLS)
    w.add_argument("--interval", type=float, default=1.0)

    doc = sub.add_parser("doctor", help="check tools, platform and memory")
    doc.set_defaults(tools=None)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if sys.platform != "darwin":
        print("agentbench's interactive comparison requires macOS. "
              "Use clanker_tests --benchmark for native in-process benchmarks "
              "on macOS and Windows.", file=sys.stderr)
        return 1

    from . import demo, doctor, measure, watch

    if args.command != "doctor":
        live = args.command in ("demo", "watch")
        args.tools = resolve(args.tools, default=LIVE_TOOLS if live else DEFAULT_TOOLS)
        if live and any(tool.name == "jcode-embed-off" for tool in args.tools):
            parser.error("jcode-embed-off is only available in measure; "
                         "live views cannot distinguish configuration variants")
    commands = {"demo": demo.main, "doctor": doctor.main,
                "measure": measure.main, "watch": watch.main}
    return commands[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
