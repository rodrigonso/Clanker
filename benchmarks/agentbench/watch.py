"""`watch` -- live RSS readout for matching agent processes."""

from __future__ import annotations

import os
import sys
import time

from . import proc

PALETTE = ["\033[92m", "\033[93m", "\033[96m", "\033[95m", "\033[91m"]
RESET = "\033[0m"
BAR_WIDTH = 38


def main(args) -> int:
    tools = args.tools
    colour = {t.name: PALETTE[i % len(PALETTE)] for i, t in enumerate(tools)}
    peak = {t.name: 0.0 for t in tools}
    width = max(len(t.name) for t in tools)

    try:
        while True:
            stats = proc.scan(tools)
            for name, (mb, _) in stats.items():
                peak[name] = max(peak[name], mb)

            live = [mb for mb, count in stats.values() if count]
            scale = max(live) if live else 1.0
            base = min((v for v in live if v > 0), default=0.0)

            out = ["\033[H\033[J", "  live memory  (RSS, matching executable images)\n\n"]
            for tool in tools:
                mb, count = stats[tool.name]
                if not count:
                    out.append(f"  {tool.name:<{width}} {'--':>9}    (not running)\n")
                    continue
                bar = int(BAR_WIDTH * mb / scale) if scale else 0
                rel = f"{mb / base:.0f}x" if base and mb > base else ""
                out.append(
                    f"  {colour[tool.name]}{tool.name:<{width}}{RESET} {mb:>7.1f} MiB  "
                    f"{colour[tool.name]}{'█' * bar}{RESET}{' ' * (BAR_WIDTH - bar)} "
                    f"{count} proc  peak {peak[tool.name]:.1f}  {rel}\n"
                )
            out.append("\n  Ctrl-b d detaches   Ctrl-b & kills the demo\n")
            sys.stdout.write("".join(out))
            sys.stdout.flush()
            time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0
