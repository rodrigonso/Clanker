"""Checks ClankAgent's NDJSON accounting against real `clank --mode json` output.

Runs without pier or harbor installed: the harness modules are stubbed with the
handful of names the adapter imports. Run with `python3 test_clank_agent.py`.
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
import tempfile
import types
from pathlib import Path
from urllib.parse import urlparse


def _install_harness_stubs() -> None:
    """Minimal stand-ins for the pier.* modules clank_agent imports."""

    class BaseInstalledAgent:
        CLI_FLAGS: list = []
        ENV_VARS: list = []

        def __init__(self, logs_dir, *args, version=None, **kwargs):
            self.logs_dir = Path(logs_dir)
            self.model_name = kwargs.get("model_name")
            self._version = version
            self._extra_env: dict[str, str] = {}
            self._resolved_flags: dict[str, object] = {}
            self.commands: list[tuple[str, str, str | None]] = []
            self.install_called = False

        def build_process_env(self, base=None, **kwargs):
            return {k: v for k, v in (base or {}).items() if v is not None}

        def _get_env(self, key):
            if key in self._extra_env:
                return self._extra_env[key]
            return os.environ.get(key)

        # Mirrors BaseInstalledAgent.setup: install() is skipped entirely when
        # the image already carries this agent's install spec.
        async def setup(self, environment):
            spec = getattr(environment, "agent_install_spec", None)
            if spec is None or getattr(spec, "agent_name", None) != self.name():
                await self.install(environment)

        async def install(self, environment):
            self.install_called = True

        async def exec_as_root(
            self, environment, command, env=None, cwd=None, timeout_sec=None
        ):
            self.commands.append(("root", command, cwd))

        async def exec_as_agent(
            self, environment, command, env=None, cwd=None, timeout_sec=None
        ):
            self.commands.append(("agent", command, cwd))

    class CliFlag:
        def __init__(self, kwarg, cli="", type="str", choices=None, **kwargs):
            self.kwarg, self.cli, self.type, self.choices = kwarg, cli, type, choices

    class Spec:
        def __init__(self, **kwargs):
            self.__dict__.update(kwargs)

    class NetworkAllowlist:
        def __init__(self, domains=None):
            self.domains = sorted({d.strip().lower() for d in (domains or [])})

    def allowlist_from_urls(values, *, default_domains=()):
        domains = {d for d in default_domains if d}
        for value in values:
            if not isinstance(value, str) or not value.strip():
                continue
            raw = value.strip()
            parsed = urlparse(raw if "://" in raw else f"https://{raw}")
            if parsed.hostname:
                domains.add(parsed.hostname.lower())
        return NetworkAllowlist(sorted(domains))

    def with_prompt_template(fn):
        return fn

    for name, attrs in {
        "pier": {},
        "pier.agents": {},
        "pier.agents.installed": {},
        "pier.agents.installed.base": {
            "BaseInstalledAgent": BaseInstalledAgent,
            "CliFlag": CliFlag,
            "with_prompt_template": with_prompt_template,
        },
        "pier.agents.network": {"allowlist_from_urls": allowlist_from_urls},
        "pier.environments": {},
        "pier.environments.base": {"BaseEnvironment": object},
        "pier.models": {},
        "pier.models.agent": {},
        "pier.models.agent.context": {"AgentContext": object},
        "pier.models.agent.network": {"NetworkAllowlist": NetworkAllowlist},
        "pier.models.agent.install": {
            "AgentInstallSpec": Spec,
            "InstallStep": Spec,
        },
    }.items():
        module = types.ModuleType(name)
        for key, value in attrs.items():
            setattr(module, key, value)
        sys.modules[name] = module


class FakeEnvironment:
    """Records uploads so setup() can be checked without a container."""

    def __init__(self, agent_install_spec=None):
        self.agent_install_spec = agent_install_spec
        self.uploads: list[tuple[str, str]] = []

    async def upload_file(self, source_path, target_path):
        self.uploads.append((str(source_path), target_path))


_install_harness_stubs()
sys.path.insert(0, str(Path(__file__).parent))
from clank_agent import ClankAgent  # noqa: E402


def _record(role: str, usage: dict | None = None) -> str:
    """A record event in the exact shape `clank --mode json` emits."""
    message: dict = {
        "role": role,
        "content": [{"kind": "text", "text": "hi", "signature": ""}],
        "timestamp": 1786455200305,
    }
    if usage is not None:
        message |= {
            "stopReason": "stop",
            "usage": usage,
            "errorMessage": "",
            "model": "claude-sonnet-4-6",
            "provider": "anthropic",
        }
    return json.dumps(
        {
            "type": "record",
            "kind": "message",
            "record": {
                "schemaVersion": 2,
                "sessionId": "ocsdKIq51pNvU27bPZT3Bg",
                "seq": 4,
                "entryId": "5hp0dcLKCCfXMSpYcIlXPg",
                "type": "message",
                "createdAt": 1786455200607,
                "payload": {"parentId": "UTaoi", "message": message},
            },
        }
    )


def _usage(**kwargs) -> dict:
    base = {
        "input": 0,
        "output": 0,
        "cacheRead": 0,
        "cacheWrite": 0,
        "reasoning": 0,
        "totalTokens": 0,
    }
    return base | kwargs


def main() -> int:
    # The adapter refuses to construct without a provider key, since the
    # container has no other credential path.
    os.environ.setdefault("ANTHROPIC_API_KEY", "test-key")
    os.environ.setdefault("OPENAI_API_KEY", "test-key")

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        binary = tmp_path / "clank"
        binary.write_bytes(b"not a real binary")

        logs = tmp_path / "logs"
        logs.mkdir()
        (logs / "clank.ndjson").write_text(
            "\n".join(
                [
                    '{"type":"agent_start","turn":"run"}',
                    _record("user"),
                    _record("assistant", _usage(input=100, output=20, cacheRead=7)),
                    "",
                    "{ not json",
                    # A tool result must not be counted as an assistant turn.
                    _record("toolResult"),
                    _record("assistant", _usage(input=50, output=5, cacheWrite=3)),
                    '{"type":"result","ok":true}',
                ]
            )
        )

        agent = ClankAgent(
            logs_dir=logs,
            model_name="anthropic/claude-sonnet-4-6",
            clank_binary=str(binary),
        )

        context = type("Ctx", (), {})()
        agent.populate_context_post_run(context)

        failures = []

        def check(label, actual, expected):
            if actual != expected:
                failures.append(f"{label}: expected {expected!r}, got {actual!r}")

        # input counts cache reads, matching how the pi adapter reports them.
        check("n_input_tokens", context.n_input_tokens, 100 + 50 + 7)
        check("n_output_tokens", context.n_output_tokens, 20 + 5)
        check("n_cache_tokens", context.n_cache_tokens, 7)
        check("version", agent._version.startswith("sha256:"), True)

        settings = json.loads(agent._settings_json())
        check("provider", settings["provider"], "anthropic")
        check("model", settings["model"], "claude-sonnet-4-6")
        check("trust", settings["defaultProjectTrust"], "always")

        openai_agent = ClankAgent(
            logs_dir=logs,
            model_name="openai-api/gpt-5.6-sol",
            clank_binary=str(binary),
            effort="xhigh",
        )
        openai_settings = json.loads(openai_agent._settings_json())
        check("openai provider", openai_settings["provider"], "openai")
        check("openai model", openai_settings["model"], "gpt-5.6-sol")
        check("effort", openai_settings.get("effort"), "xhigh")
        # Absent unless asked for, so clank keeps its own defaults.
        check("no retry block by default", "retry" in openai_settings, False)

        # Retry is a settings.json block, not a flag; a long concurrent run
        # needs more than clank's default 3 attempts at 8s to survive 429s.
        retrying = ClankAgent(
            logs_dir=logs,
            model_name="anthropic/claude-sonnet-4-6",
            clank_binary=str(binary),
            max_retries="8",
            retry_max_delay_ms="60000",
        )
        check(
            "retry block",
            json.loads(retrying._settings_json())["retry"],
            {"maxRetries": 8, "maxDelayMs": 60000},
        )

        # An unknown effort must fail at construction, not silently produce a
        # settings.json that clank rejects as malformed mid-trial.
        try:
            ClankAgent(
                logs_dir=logs,
                model_name="anthropic/claude-sonnet-4-6",
                clank_binary=str(binary),
                effort="turbo",
            )
            failures.append("bad effort: expected ValueError, got none")
        except ValueError:
            pass

        # Likewise for a provider clank does not implement.
        try:
            ClankAgent(
                logs_dir=logs,
                model_name="google/gemini-3-pro",
                clank_binary=str(binary),
            )._settings_json()
            failures.append("bad provider: expected ValueError, got none")
        except ValueError:
            pass

        # A missing log must not raise; the trial may have died before writing.
        empty = ClankAgent(
            logs_dir=tmp_path / "nonexistent",
            model_name="anthropic/claude-sonnet-4-6",
            clank_binary=str(binary),
        )
        blank = type("Ctx", (), {})()
        empty.populate_context_post_run(blank)
        check("missing log is inert", hasattr(blank, "n_input_tokens"), False)

        # Regression: DeepSWE tasks are air-gapped, so an empty allowlist means
        # every model call fails with no route to host.
        check(
            "allowlist anthropic",
            agent.network_allowlist().domains,
            ["api.anthropic.com"],
        )
        check(
            "allowlist openai",
            openai_agent.network_allowlist().domains,
            ["api.openai.com"],
        )

        gateway = ClankAgent(
            logs_dir=logs,
            model_name="openai-api/gpt-5.6-sol",
            clank_binary=str(binary),
        )
        gateway._extra_env["OPENAI_BASE_URL"] = "https://gateway.internal/v1"
        check(
            "allowlist honours base url",
            gateway.network_allowlist().domains,
            ["api.openai.com", "gateway.internal"],
        )

        # Regression: the binary upload used to live in install(), which Pier
        # skips whenever the install spec is baked into the image.
        lib = tmp_path / "libssl.so.3"
        lib.write_bytes(b"stub")
        uploader = ClankAgent(
            logs_dir=logs,
            model_name="anthropic/claude-sonnet-4-6",
            clank_binary=str(binary),
            clank_libs=str(lib),
        )
        preinstalled = FakeEnvironment(
            agent_install_spec=type("S", (), {"agent_name": "clank"})()
        )
        asyncio.run(uploader.setup(preinstalled))
        check("install skipped when preinstalled", uploader.install_called, False)
        check(
            "binary uploaded anyway",
            [target for _, target in preinstalled.uploads],
            ["/usr/local/bin/clank", "/opt/clank/lib/libssl.so.3"],
        )

        # Regression: a missing provider key must fail at construction, before
        # a sandbox is built and a task started.
        saved = os.environ.pop("ANTHROPIC_API_KEY")
        try:
            ClankAgent(
                logs_dir=logs,
                model_name="anthropic/claude-sonnet-4-6",
                clank_binary=str(binary),
            )
            failures.append("missing key: expected ValueError, got none")
        except ValueError:
            pass
        finally:
            os.environ["ANTHROPIC_API_KEY"] = saved

        for failure in failures:
            print(f"FAIL {failure}")
        if failures:
            return 1
        print("ok: clank_agent NDJSON accounting and settings mapping")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
