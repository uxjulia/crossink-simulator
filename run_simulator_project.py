"""
Project-side PlatformIO hook for exposing the IDE-visible "Run Simulator" task.

Use this from a consuming firmware repo when you want the PlatformIO IDE to show
the simulator under Project Tasks > simulator > Custom > Run Simulator.

The simulator library already auto-loads `run_simulator.py` through
`library.json`. That library hook handles compatibility patching and keeps the
CLI target available for older consumers. This separate project hook exists so
the consuming repo can own the IDE-visible task registration without causing the
same launcher script to be loaded twice.
"""

Import("env")  # noqa: F821 - SCons injects this at build time
import builtins

RUN_SIMULATOR_TARGET_KEY = "_crosspoint_run_simulator_target_registered"
SIMULATOR_HTTP_PORT_OPTION = "custom_simulator_http_port"


def run_simulator(source, target, env):
    import os
    import subprocess

    binary = env.subst("$BUILD_DIR/program")
    runtime_env = os.environ.copy()
    configured_http_port = env.GetProjectOption(
        SIMULATOR_HTTP_PORT_OPTION, ""
    ).strip()
    if configured_http_port:
        runtime_env["CROSSPOINT_SIM_HTTP_PORT"] = configured_http_port
    return subprocess.run([binary], cwd=os.getcwd(), env=runtime_env).returncode


def run_existing_simulator(source, target, env):
    import os
    import subprocess

    binary = env.subst("$BUILD_DIR/program")
    if not os.path.isfile(binary):
        print(f"Simulator binary not found: {binary}")
        print(f"Build it first with: pio run -e {env.subst('$PIOENV')}")
        return 1

    return subprocess.run([binary], cwd=os.getcwd()).returncode


if not getattr(builtins, RUN_SIMULATOR_TARGET_KEY, False):
    setattr(builtins, RUN_SIMULATOR_TARGET_KEY, True)
    env.AddCustomTarget(
        name="run_simulator",
        dependencies="$PROGPATH",
        actions=run_simulator,
        title="Run Simulator",
        description="Build and run the desktop simulator",
        always_build=True,
    )
    env.AddCustomTarget(
        name="run_simulator_no_build",
        dependencies=None,
        actions=run_existing_simulator,
        title="Run Simulator (No Build)",
        description="Run the existing desktop simulator binary without rebuilding",
        always_build=True,
    )
