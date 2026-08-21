"""Clanker as a Pier / Harbor agent.

[Pier](https://github.com/datacurve-ai/pier) runs DeepSWE; Harbor runs
Terminal-Bench. Both resolve a custom agent from a ``module:ClassName`` import
path and expose the same ``BaseInstalledAgent`` surface, so one adapter serves
both. Clanker is a compiled binary with no package registry to install from, so
it is uploaded from the host rather than fetched by ``install_spec``.

    pier run -p deep-swe/tasks --agent-import-path clanker_agent:ClankerAgent ...
"""

from __future__ import annotations

import hashlib
import json
import os
import shlex
import sys
from pathlib import Path
from typing import Any

try:  # Pier and Harbor expose the same API under different roots.
    from pier.agents.installed.base import BaseInstalledAgent, with_prompt_template
    from pier.environments.base import BaseEnvironment
    from pier.models.agent.context import AgentContext
    from pier.models.agent.install import AgentInstallSpec, InstallStep
except ImportError:  # pragma: no cover - exercised only under Harbor
    from harbor.agents.installed.base import BaseInstalledAgent, with_prompt_template
    from harbor.environments.base import BaseEnvironment
    from harbor.models.agent.context import AgentContext
    from harbor.models.agent.install import AgentInstallSpec, InstallStep

try:  # Egress filtering is a Pier addition; Harbor may not carry it.
    from pier.agents.network import allowlist_from_urls
except ImportError:  # pragma: no cover
    try:
        from harbor.agents.network import allowlist_from_urls
    except ImportError:
        allowlist_from_urls = None

CONTAINER_BIN = "/usr/local/bin/clanker"
CONTAINER_LIB_DIR = "/opt/clanker/lib"
CONTAINER_HOME = "/tmp/clanker-home"

#                api domain        key env                base-url overrides
PROVIDERS = {
    "openrouter": (
        "openrouter.ai",
        "OPENROUTER_API_KEY",
        ("OPENROUTER_BASE_URL",),
    ),
}
BASE_URL_ENV = tuple(url for _, _, urls in PROVIDERS.values() for url in urls)
FORWARDED_ENV = tuple(key for _, key, _ in PROVIDERS.values()) + BASE_URL_ENV

EFFORTS = ("low", "medium", "high", "xhigh", "max", "ultra")
DEFAULT_MAX_RETRIES = 8
DEFAULT_RETRY_MAX_DELAY_MS = 60000
MAX_U32 = (1 << 32) - 1
MAX_RETRIES = 1000


def _bounded_int(
    name: str,
    value: int | str | None,
    default: int,
    minimum: int,
    maximum: int,
) -> int:
    parsed = default if value is None else int(value)
    if parsed < minimum or parsed > maximum:
        raise ValueError(f"{name} must be between {minimum} and {maximum}")
    return parsed


class ClankerAgent(BaseInstalledAgent):
    """Runs ``clanker -p <instruction> --mode json`` inside the task container."""

    SUPPORTS_ATIF = False
    _OUTPUT_FILENAME = "clanker.ndjson"

    def __init__(
        self,
        *args,
        clanker_binary: str | None = None,
        clanker_libs: list[str] | str | None = None,
        workdir: str | None = None,
        effort: str | None = None,
        max_retries: int | str | None = None,
        retry_max_delay_ms: int | str | None = None,
        **kwargs,
    ):
        super().__init__(*args, **kwargs)
        binary = clanker_binary or os.environ.get("CLANKER_BIN")
        if not binary:
            raise ValueError(
                "ClankerAgent needs the host path of a Linux x86_64 clanker binary. "
                "Pass --agent-kwarg clanker_binary=/path/to/clanker or set CLANKER_BIN."
            )
        self._binary = Path(binary).expanduser().resolve()
        if not self._binary.is_file():
            raise ValueError(f"clanker binary not found: {self._binary}")

        if isinstance(clanker_libs, str):
            clanker_libs = [p for p in clanker_libs.split(",") if p]
        self._libs = [Path(p).expanduser().resolve() for p in (clanker_libs or [])]
        self._workdir = workdir

        # Not declared as a CliFlag: clanker reads effort from settings.json, so
        # declaring one would leak a bare argument onto the command line.
        if effort is not None and effort not in EFFORTS:
            raise ValueError(
                f"unknown effort '{effort}'; expected one of {', '.join(EFFORTS)}"
            )
        self._effort = effort

        # A request retry preserves the agent's context and prompt-cache state.
        # Pier's trial retry deletes the failed trial directory and starts over,
        # so benchmark runs keep the wider retry budget here instead.
        self._max_retries = _bounded_int(
            "max_retries", max_retries, DEFAULT_MAX_RETRIES, 0, MAX_RETRIES
        )
        self._retry_max_delay_ms = _bounded_int(
            "retry_max_delay_ms",
            retry_max_delay_ms,
            DEFAULT_RETRY_MAX_DELAY_MS,
            1,
            MAX_U32,
        )

        # clanker has no --version, so the binary's digest is the only honest
        # build identifier available for the trial metadata.
        if self._version is None:
            digest = hashlib.sha256(self._binary.read_bytes()).hexdigest()
            self._version = f"sha256:{digest[:12]}"

        # The container gets a fresh HOME with no auth.json, so the API key is
        # the only credential path. Checked now because otherwise the missing
        # key surfaces only after a sandbox is built and the task started.
        try:
            provider, _ = self._parsed_provider_model()
        except ValueError:
            provider = None
        if provider and not self._get_env(PROVIDERS[provider][1]):
            raise ValueError(
                f"{PROVIDERS[provider][1]} is not set, so clanker cannot "
                f"authenticate to {provider} inside the task container. "
                f"Export it before running, or pass it as job-level agent env."
            )

    @staticmethod
    def name() -> str:
        return "clanker"

    def get_version_command(self) -> str | None:
        return None  # Version comes from the host-side digest.

    def install_spec(self) -> AgentInstallSpec:
        # Image-build steps only. The binary itself is uploaded in setup(), so
        # nothing here may assume it exists.
        return AgentInstallSpec(
            agent_name=self.name(),
            version=self._version,
            steps=[
                InstallStep(
                    user="root",
                    env={"DEBIAN_FRONTEND": "noninteractive"},
                    run=(
                        "set -eu; "
                        "if command -v apt-get >/dev/null 2>&1; then "
                        "  apt-get update && apt-get install -y --no-install-recommends "
                        "    ca-certificates git ripgrep; "
                        "elif command -v apk >/dev/null 2>&1; then "
                        "  apk add --no-cache ca-certificates git ripgrep; "
                        "fi; "
                        f"mkdir -p {CONTAINER_LIB_DIR} {CONTAINER_HOME}"
                    ),
                ),
            ],
            verification_command=f"test -d {CONTAINER_LIB_DIR} && command -v git",
        )

    async def setup(self, environment: BaseEnvironment) -> None:
        # The upload has to happen here, not in install(): when Pier bakes the
        # install spec into the image, BaseInstalledAgent.setup() sees the agent
        # as preinstalled and never calls install() at all.
        await super().setup(environment)

        await environment.upload_file(self._binary, CONTAINER_BIN)
        for lib in self._libs:
            await environment.upload_file(lib, f"{CONTAINER_LIB_DIR}/{lib.name}")

        await self.exec_as_root(
            environment,
            command=(
                f"mkdir -p {CONTAINER_LIB_DIR} {CONTAINER_HOME} && "
                f"chmod 0755 {CONTAINER_BIN} && "
                f"chmod -R a+rX {CONTAINER_LIB_DIR} && "
                f"chmod 0777 {CONTAINER_HOME}"
            ),
        )

        # Fail here rather than mid-task: a libc or libssl mismatch otherwise
        # shows up as a silent non-zero exit once the trial is already running.
        await self.exec_as_agent(
            environment,
            command=f"{CONTAINER_BIN} --help >/dev/null",
            env=self._runtime_env(),
        )

    def network_allowlist(self):
        """Domains Clanker must reach while the task itself is air-gapped.

        DeepSWE runs the agent with ``network_mode = "no-network"`` and Pier
        opens an egress proxy only for domains declared here, so inheriting the
        empty default fails every model call with no route to host.
        """
        urls = [value for key in BASE_URL_ENV if (value := self._get_env(key))]

        try:
            defaults = [PROVIDERS[self._parsed_provider_model()[0]][0]]
        except ValueError:
            defaults = sorted(domain for domain, _, _ in PROVIDERS.values())

        if allowlist_from_urls is None:  # pragma: no cover
            return super().network_allowlist()
        return allowlist_from_urls(urls, default_domains=defaults)

    def _runtime_env(self) -> dict[str, str]:
        base: dict[str, str | None] = {
            "HOME": CONTAINER_HOME,
            "CLANKER_OFFLINE": "1",
            "TERM": "dumb",
            "NO_COLOR": "1",
        }
        if self._libs:
            base["LD_LIBRARY_PATH"] = CONTAINER_LIB_DIR
        for key in FORWARDED_ENV:
            if value := self._get_env(key):
                base[key] = value
        return self.build_process_env(base)

    def _parsed_provider_model(self) -> tuple[str, str]:
        if not self.model_name or "/" not in self.model_name:
            raise ValueError(
                "model_name must be an OpenRouter model id, "
                "e.g. anthropic/claude-sonnet-4.5"
            )
        return "openrouter", self.model_name

    def _settings_json(self) -> str:
        """Clanker takes its OpenRouter model and effort from settings.json."""
        provider, model = self._parsed_provider_model()
        settings: dict[str, Any] = {
            "provider": provider,
            "model": model,
            # Benchmark repos ship no .clanker/, so trust never actually gates
            # here; set anyway so a task that does ship one is not silently
            # stripped of its prompt files in a non-interactive run.
            "defaultProjectTrust": "always",
        }
        if self._effort:
            settings["effort"] = self._effort
        retry: dict[str, Any] = {}
        retry["maxRetries"] = self._max_retries
        retry["maxDelayMs"] = self._retry_max_delay_ms
        settings["retry"] = retry
        return json.dumps(settings)

    @with_prompt_template
    async def run(
        self,
        instruction: str,
        environment: BaseEnvironment,
        context: AgentContext,
    ) -> None:
        env = self._runtime_env()

        await self.exec_as_agent(
            environment,
            command=(
                f"mkdir -p {CONTAINER_HOME}/.clanker && "
                f"printf '%s' {shlex.quote(self._settings_json())} "
                f"> {CONTAINER_HOME}/.clanker/settings.json"
            ),
            env=env,
        )

        # clanker has no -C/--cwd; the working directory is set on exec instead,
        # and left unset by default so the task image's own WORKDIR applies.
        await self.exec_as_agent(
            environment,
            command=(
                f"{CONTAINER_BIN} --mode json --approve -p {shlex.quote(instruction)} "
                f"</dev/null 2>/logs/agent/clanker.stderr "
                f"| stdbuf -oL tee /logs/agent/{self._OUTPUT_FILENAME}"
            ),
            cwd=self._workdir,
            env=env,
        )

    def populate_context_post_run(self, context: AgentContext) -> None:
        output = self.logs_dir / self._OUTPUT_FILENAME
        if not output.exists():
            return

        totals = {"input": 0, "output": 0, "cacheRead": 0, "cacheWrite": 0}
        final_stop = None
        for line in output.read_text(errors="replace").splitlines():
            if not line.strip():
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if event.get("type") != "record" or event.get("kind") != "message":
                continue
            message = (
                (event.get("record") or {}).get("payload", {}).get("message") or {}
            )
            if message.get("role") != "assistant":
                continue
            usage = message.get("usage") or {}
            for key in totals:
                totals[key] += usage.get(key, 0) or 0
            final_stop = message.get("stopReason")

        context.n_input_tokens = totals["input"] + totals["cacheRead"]
        context.n_output_tokens = totals["output"]
        context.n_cache_tokens = totals["cacheRead"]
        # Clanker does not price requests, so cost is left for the harness to
        # derive from token counts rather than reported as zero.

        # A truncated run did not decide it was finished, it was cut off --
        # most likely before committing, which grades identically to a wrong
        # answer. Current clanker fails the run outright; older builds exit clean.
        if final_stop == "length":
            print(
                "clanker_agent: WARNING - final assistant message stopped on "
                "'length' (output token cap). This trial was truncated, not "
                "completed; its score does not reflect answer quality.",
                file=sys.stderr,
            )
