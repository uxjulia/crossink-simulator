#!/usr/bin/env python3
"""Build/run actual FreeRTOS task-shim overflow probes; no SDL required."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="sim-stack-tests-") as temp:
    binary = Path(temp) / "probe"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O1", "-pthread",
                    "-finstrument-functions", "-fno-omit-frame-pointer",
                    "-fno-optimize-sibling-calls", "-DCROSSPOINT_SIM_STACK_CHECK",
                    "-I" + str(root / "src"), str(root / "tests/stack_check.cpp"),
                    str(root / "src/SimulatorStackCheck.cpp"), "-ldl", "-o", str(binary)], check=True)
    for api in ("normal", "static", "pinned"):
        for mode, scenario, expected in (("fail", "small", 0), ("fail", "medium", 0), ("fail", "large", 86),
                                         ("fail", "nested", 86), ("warn", "large", 0),
                                         ("off", "large", 0)):
            result = subprocess.run([str(binary), scenario, api], capture_output=True, text=True,
                                    env={**os.environ, "CROSSPOINT_SIM_STACK_CHECK": mode}, timeout=10)
            assert result.returncode == expected, (api, mode, scenario, result.stderr)
            if mode != "off" and scenario in ("large", "nested"):
                assert "task=StackProbe" in result.stderr and "budget=16384 bytes" in result.stderr
            else:
                assert "task=StackProbe" not in result.stderr
    result = subprocess.run([str(binary), "large", "normal"], capture_output=True, text=True,
                            env={**os.environ, "CROSSPOINT_SIM_STACK_CHECK": "invalid"}, timeout=10)
    assert result.returncode == 86 and "Invalid mode" in result.stderr
    for api in ("normal", "static", "pinned"):
        result = subprocess.run([str(binary), "medium", api, "8192"], capture_output=True, text=True,
                                env={**os.environ, "CROSSPOINT_SIM_STACK_CHECK": "fail"}, timeout=10)
        assert result.returncode == 86 and "budget=8192 bytes" in result.stderr
    clean_env = dict(os.environ)
    clean_env.pop("CROSSPOINT_SIM_STACK_CHECK", None)
    result = subprocess.run([str(binary), "large", "normal"], capture_output=True, text=True,
                            env=clean_env, timeout=10)
    assert result.returncode == 86 and "task=StackProbe" in result.stderr
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O1", "-pthread",
                    "-I" + str(root / "src"), str(root / "tests/stack_check.cpp"),
                    str(root / "src/SimulatorStackCheck.cpp"), "-ldl", "-o", str(binary)], check=True)
    result = subprocess.run([str(binary), "large", "normal"], capture_output=True, text=True,
                            env={**os.environ, "CROSSPOINT_SIM_STACK_CHECK": "fail"}, timeout=10)
    assert result.returncode == 86 and "require a rebuild" in result.stderr
    result = subprocess.run([str(binary), "large", "normal"], capture_output=True, text=True,
                            env=clean_env, timeout=10)
    assert result.returncode == 0 and "[SIM STACK]" not in result.stderr
    result = subprocess.run([str(binary), "large", "normal"], capture_output=True, text=True,
                            env={**clean_env,
                                 "CROSSINK_SIMULATOR_STACK_BUDGETS": "StackProbe=8192"}, timeout=10)
    assert result.returncode == 0, result.stderr
    assert "SIMSTACK task=StackProbe requested=16384 expected=8192" in result.stderr
    assert "SIMSTACK BUDGET_BREACH task=StackProbe requested=16384 budget=8192" in result.stderr
    # Exercise a detached task after its public handle has been destroyed.
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O1", "-pthread",
                    "-finstrument-functions", "-fno-omit-frame-pointer",
                    "-fno-optimize-sibling-calls", "-DCROSSPOINT_SIM_STACK_CHECK",
                    "-fsanitize=address,undefined", "-I" + str(root / "src"),
                    str(root / "tests/stack_check.cpp"), str(root / "src/SimulatorStackCheck.cpp"),
                    "-ldl", "-o", str(binary)], check=True)
    result = subprocess.run([str(binary), "detach", "normal"], capture_output=True, text=True,
                            env={**os.environ, "CROSSPOINT_SIM_STACK_CHECK": "fail"}, timeout=10)
    assert result.returncode == 0, result.stderr
    print("PASS: task-stack checks including profile budgets and ASan/UBSan detached-task lifetime")
