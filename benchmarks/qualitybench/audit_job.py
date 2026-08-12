"""Audit a Pier job for benchmark-integrity failures and estimate API cost."""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Pricing:
    Input: float
    Output: float
    CacheRead: float
    CacheWrite: float


PRICING_PER_MILLION = {
    "claude-sonnet-4-6": Pricing(3.0, 15.0, 0.30, 3.75),
    "claude-opus-4-8": Pricing(5.0, 25.0, 0.50, 6.25),
}


def _json(path: Path) -> dict:
    return json.loads(path.read_text())


def _ndjson_summary(path: Path) -> tuple[dict[str, int], str | None, int]:
    usage = {"input": 0, "output": 0, "cacheRead": 0, "cacheWrite": 0}
    final_stop = None
    retries = 0
    for raw in path.read_text(errors="replace").splitlines():
        try:
            event = json.loads(raw)
        except json.JSONDecodeError:
            continue
        if event.get("type") == "retry_start":
            retries += 1
        if event.get("type") != "record" or event.get("kind") != "message":
            continue
        message = ((event.get("record") or {}).get("payload") or {}).get("message") or {}
        if message.get("role") != "assistant":
            continue
        turn_usage = message.get("usage") or {}
        for key in usage:
            usage[key] += turn_usage.get(key, 0) or 0
        final_stop = message.get("stopReason")
    return usage, final_stop, retries


def audit(job: Path) -> dict:
    job = job.resolve()
    result_path = job / "result.json"
    if not result_path.is_file():
        raise ValueError(f"job result not found: {result_path}")
    result = _json(result_path)
    stats = result.get("stats") or {}
    total_trials = result.get("n_total_trials", 0)
    completed_trials = stats.get("n_completed_trials", 0)
    issues: list[str] = []
    warnings: list[str] = []
    usage = {"input": 0, "output": 0, "cacheRead": 0, "cacheWrite": 0}
    request_retries = 0
    model = None
    clank_trials = 0
    empty_patches = 0

    pier_retries = stats.get("n_retries", 0) or 0
    if pier_retries:
        issues.append(
            f"{pier_retries} Pier trial retries deleted failed-attempt evidence"
        )
    errored = stats.get("n_errored_trials", 0) or 0
    if errored:
        issues.append(f"{errored} trials ended with exceptions")
    if completed_trials != total_trials:
        issues.append(
            f"job is incomplete: {completed_trials}/{total_trials} trials completed"
        )

    for trial_dir in sorted(path for path in job.iterdir() if path.is_dir()):
        trial_result_path = trial_dir / "result.json"
        if not trial_result_path.is_file():
            continue
        trial = _json(trial_result_path)
        agent = trial.get("agent_info") or {}
        if agent.get("name") != "clank":
            continue
        clank_trials += 1
        trial_name = trial.get("trial_name") or trial_dir.name
        trial_model = ((agent.get("model_info") or {}).get("name"))
        if model is None:
            model = trial_model
        elif trial_model != model:
            warnings.append(f"{trial_name}: model differs from other trials")

        if trial.get("exception_info"):
            issues.append(f"{trial_name}: agent exception")

        patch = trial_dir / "artifacts" / "model.patch"
        if not patch.is_file():
            issues.append(f"{trial_name}: model.patch is missing")
        elif patch.stat().st_size == 0:
            empty_patches += 1

        events = trial_dir / "agent" / "clank.ndjson"
        if not events.is_file():
            issues.append(f"{trial_name}: clank.ndjson is missing")
            continue
        trial_usage, final_stop, retries = _ndjson_summary(events)
        for key in usage:
            usage[key] += trial_usage[key]
        request_retries += retries
        if final_stop is None:
            issues.append(f"{trial_name}: no final assistant stop reason")
        elif final_stop == "length":
            issues.append(f"{trial_name}: final assistant message was truncated")

    evals = stats.get("evals") or {}
    if any(name.startswith("clank__") for name in evals):
        if clank_trials != completed_trials:
            issues.append(
                f"found evidence for {clank_trials}/{completed_trials} completed "
                "Clank trials"
            )
    if empty_patches:
        warnings.append(
            f"{empty_patches} Clank trials produced an empty model.patch and scored zero"
        )

    pricing = PRICING_PER_MILLION.get(model)
    estimated_cost = None
    if pricing:
        estimated_cost = (
            usage["input"] * pricing.Input
            + usage["output"] * pricing.Output
            + usage["cacheRead"] * pricing.CacheRead
            + usage["cacheWrite"] * pricing.CacheWrite
        ) / 1_000_000
    elif model:
        warnings.append(f"no pricing configured for {model}")

    return {
        "job": str(job),
        "total_trials": total_trials,
        "completed_trials": completed_trials,
        "errored_trials": errored,
        "pier_retries": pier_retries,
        "clank_trials": clank_trials,
        "request_retries": request_retries,
        "empty_patches": empty_patches,
        "model": model,
        "usage": usage,
        "estimated_cost_usd": estimated_cost,
        "issues": issues,
        "warnings": warnings,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("job", type=Path)
    args = parser.parse_args(argv)
    try:
        report = audit(args.job)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))

    print(
        f"trials: {report['completed_trials']}/{report['total_trials']} completed, "
        f"{report['errored_trials']} exceptions"
    )
    print(
        f"retries: {report['request_retries']} Clank request, "
        f"{report['pier_retries']} Pier trial"
    )
    usage = report["usage"]
    print(
        "tokens: "
        f"{usage['input']} input, {usage['cacheRead']} cache read, "
        f"{usage['cacheWrite']} cache write, {usage['output']} output"
    )
    if report["estimated_cost_usd"] is not None:
        print(
            f"estimated API cost: ${report['estimated_cost_usd']:.2f} "
            "(5-minute cache-write pricing)"
        )
    for warning in report["warnings"]:
        print(f"WARNING: {warning}")
    for issue in report["issues"]:
        print(f"FAIL: {issue}")
    if report["issues"]:
        return 1
    print("audit: clean")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
