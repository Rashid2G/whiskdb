#!/usr/bin/env python3
"""WhiskDB Persistence Benchmark Branch Runner & Comparison Tool.

Runs the standard WhiskDB persistence benchmark against a specified branch
(e.g., master, main, or dev) and compares throughput and latency across all
durability modes (Strict, GroupCommit, Relaxed, MemoryOnly).

Generates terminal tables, structured JSON metrics, and PR-ready GitHub Markdown
reports to help maintainers merge or reject pull requests based on performance impact.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Dict, List, Optional, Tuple


def get_toolchain_env() -> Dict[str, str]:
    env = os.environ.copy()
    whisk_tools = os.path.expanduser("~/.whisk-tools")
    if os.path.isdir(whisk_tools):
        bin_dir = os.path.join(whisk_tools, "bin")
        wrap_dir = os.path.join(whisk_tools, "wrap")
        env["PATH"] = f"{bin_dir}:{wrap_dir}:{env.get('PATH', '')}"
        env["CC"] = os.path.join(wrap_dir, "zcc")
        env["CXX"] = os.path.join(wrap_dir, "zcxx")
    return env


def get_cmake_tool_args(env: Dict[str, str]) -> List[str]:
    args = []
    whisk_tools = os.path.expanduser("~/.whisk-tools")
    if os.path.isdir(whisk_tools):
        wrap_dir = os.path.join(whisk_tools, "wrap")
        args.extend([
            f"-DCMAKE_C_COMPILER={os.path.join(wrap_dir, 'zcc')}",
            f"-DCMAKE_CXX_COMPILER={os.path.join(wrap_dir, 'zcxx')}",
            f"-DCMAKE_AR={os.path.join(wrap_dir, 'zar')}",
            f"-DCMAKE_RANLIB={os.path.join(wrap_dir, 'zranlib')}",
        ])
    return args


def run_cmd(
    cmd: List[str],
    cwd: Optional[str] = None,
    env: Optional[Dict[str, str]] = None,
    capture: bool = False,
    check: bool = True,
) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=cwd,
        env=env or get_toolchain_env(),
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        check=check,
    )


def resolve_git_ref(ref: str, repo_dir: str) -> Optional[str]:
    for candidate in [ref, f"origin/{ref}", f"refs/heads/{ref}", f"refs/remotes/origin/{ref}"]:
        res = run_cmd(["git", "rev-parse", "--verify", candidate], cwd=repo_dir, capture=True, check=False)
        if res.returncode == 0:
            return res.stdout.strip()
    return None


def get_git_commit_info(ref: str, repo_dir: str) -> str:
    res = run_cmd(["git", "log", "-1", "--format=%h (\"%s\")", ref], cwd=repo_dir, capture=True, check=False)
    if res.returncode == 0:
        return res.stdout.strip()
    return ref


def build_bench_executable(source_dir: str, build_dir: str, repo_root: str) -> str:
    env = get_toolchain_env()
    tool_args = get_cmake_tool_args(env)
    ninja_path = shutil.which("ninja", path=env.get("PATH"))
    deps_dir = os.path.join(repo_root, "build", "_deps")

    cmake_args = [
        "cmake",
        "-S", source_dir,
        "-B", build_dir,
        "-DCMAKE_BUILD_TYPE=Release",
    ]
    if os.path.isdir(deps_dir):
        cmake_args.append(f"-DFETCHCONTENT_BASE_DIR={deps_dir}")
    if ninja_path:
        cmake_args.extend(["-G", "Ninja"])
    cmake_args.extend(tool_args)

    print(f"==> Configuring in {build_dir}...")
    run_cmd(cmake_args, cwd=source_dir, env=env)

    print(f"==> Building whisk_bench in {build_dir}...")
    run_cmd(["cmake", "--build", build_dir, "--target", "whisk_bench", "-j"], cwd=source_dir, env=env)

    bench_bin = os.path.join(build_dir, "whisk_bench")
    if not os.path.isfile(bench_bin):
        bench_bin = os.path.join(build_dir, "app", "whisk_bench")
    if not os.path.isfile(bench_bin):
        raise RuntimeError(f"whisk_bench binary not found in {build_dir}")
    return bench_bin


def run_benchmark(
    bench_bin: str,
    output_json: str,
    quick: bool = True,
    mode: str = "all",
    ops: int = 0,
    threads: int = 4,
) -> Dict[str, Any]:
    cmd = [bench_bin, "--mode", mode, "--json", output_json]
    if quick:
        cmd.append("--quick")
    if ops > 0:
        cmd.extend(["--ops", str(ops)])
    if threads > 0:
        cmd.extend(["--threads", str(threads)])

    print(f"==> Running: {' '.join(cmd)}")
    run_cmd(cmd)

    with open(output_json, "r") as f:
        return json.load(f)


def format_delta(val: float, is_latency: bool = False, abs_diff_us: float = 0.0) -> Tuple[str, str]:
    """Returns formatted string and status (improved, neutral, regressed)."""
    good = val < 0 if is_latency else val > 0
    bad = val > 0 if is_latency else val < 0

    abs_val = abs(val)
    sign = "+" if val > 0 else ""
    text = f"{sign}{val:.1f}%"

    # Microsecond-level noise filter for latency
    if is_latency and abs_diff_us < 5.0:
        return text, "neutral"

    if abs_val < 3.0:
        return text, "neutral"
    if good:
        return text, "improved"
    if bad and abs_val >= 10.0:
        return text, "regressed"
    return text, "neutral"


def generate_markdown_report(
    target_name: str,
    target_info: str,
    base_name: str,
    base_info: str,
    target_data: Optional[Dict[str, Any]],
    base_data: Dict[str, Any],
    threshold: float,
) -> Tuple[str, bool]:
    has_target = target_data is not None and "metrics" in target_data
    base_metrics = base_data.get("metrics", [])

    has_regression = False
    regressions_list: List[str] = []

    lines = []
    lines.append("# ⚡ WhiskDB Performance Benchmark Report")
    lines.append("")
    lines.append(f"**Target Branch (Baseline):** `{target_name}` ({target_info})  ")
    lines.append(f"**Current PR (Candidate):** `{base_name}` ({base_info})  ")
    lines.append(f"**Regression Alert Threshold:** `{threshold:.1f}%`  ")
    lines.append("")

    if not has_target:
        lines.append("> [!NOTE]")
        lines.append(f"> Baseline branch `{target_name}` does not have the database engine or benchmark binary.")
        lines.append(f"> Initial performance baseline established for PR branch `{base_name}`.")
        lines.append("")
        lines.append("### Baseline Metrics Across Persistence Modes")
        lines.append("")
        lines.append("| Mode | Workload | Ops | Throughput (ops/sec) | Avg Latency (µs) | p50 Latency (µs) | p99 Latency (µs) |")
        lines.append("| :--- | :--- | :---: | :---: | :---: | :---: | :---: |")
        for m in base_metrics:
            lines.append(
                f"| `{m['mode']}` | `{m['workload']}` | {m['total_ops']} | "
                f"**{m['ops_per_sec']:,.1f}** | {m['avg_lat_us']:,.1f} | "
                f"{m['p50_lat_us']:,.1f} | {m['p99_lat_us']:,.1f} |"
            )
        lines.append("")
        lines.append("### 🏁 Maintainer Verdict")
        lines.append("🟢 **APPROVED — Initial Engine Baseline Established.** All persistence modes verified.")
        return "\n".join(lines), False

    # Map target metrics by (mode, workload)
    target_map = {(m["mode"], m["workload"]): m for m in target_data.get("metrics", [])}

    rows = []
    for b in base_metrics:
        key = (b["mode"], b["workload"])
        t = target_map.get(key)
        if not t:
            continue

        # Throughput delta
        t_ops = t["ops_per_sec"]
        b_ops = b["ops_per_sec"]
        t_delta = ((b_ops - t_ops) / t_ops * 100.0) if t_ops > 0 else 0.0
        t_delta_str, t_status = format_delta(t_delta, is_latency=False)

        # p50 Latency delta
        t_p50 = t["p50_lat_us"]
        b_p50 = b["p50_lat_us"]
        p50_diff = abs(b_p50 - t_p50)
        p50_delta = ((b_p50 - t_p50) / t_p50 * 100.0) if t_p50 > 0 else 0.0
        p50_delta_str, p50_status = format_delta(p50_delta, is_latency=True, abs_diff_us=p50_diff)

        # p99 Latency delta
        t_p99 = t["p99_lat_us"]
        b_p99 = b["p99_lat_us"]
        p99_diff = abs(b_p99 - t_p99)
        p99_delta = ((b_p99 - t_p99) / t_p99 * 100.0) if t_p99 > 0 else 0.0
        p99_delta_str, p99_status = format_delta(p99_delta, is_latency=True, abs_diff_us=p99_diff)

        status_icon = "⚪"
        if t_status == "regressed" or p50_status == "regressed":
            status_icon = "🔴"
            has_regression = True
            regressions_list.append(f"`{b['mode']}:{b['workload']}`")
        elif t_status == "improved" or p50_status == "improved":
            status_icon = "🟢"

        rows.append({
            "mode": b["mode"],
            "workload": b["workload"],
            "icon": status_icon,
            "target_ops": t_ops,
            "base_ops": b_ops,
            "delta_ops": t_delta_str,
            "target_p50": t_p50,
            "base_p50": b_p50,
            "delta_p50": p50_delta_str,
            "target_p99": t_p99,
            "base_p99": b_p99,
            "delta_p99": p99_delta_str,
        })

    lines.append("### 📈 Persistence Modes Throughput & Latency Comparison")
    lines.append("")
    lines.append("| Status | Mode | Workload | Base Throughput | PR Throughput | Throughput Δ | Base p50 (µs) | PR p50 (µs) | p50 Δ | PR p99 (µs) |")
    lines.append("| :---: | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |")
    for r in rows:
        lines.append(
            f"| {r['icon']} | `{r['mode']}` | `{r['workload']}` | "
            f"{r['target_ops']:,.1f} | {r['base_ops']:,.1f} | **{r['delta_ops']}** | "
            f"{r['target_p50']:,.1f} | {r['base_p50']:,.1f} | {r['delta_p50']} | "
            f"{r['base_p99']:,.1f} |"
        )
    lines.append("")
    lines.append("---")
    lines.append("")
    lines.append("### 🏁 Maintainer Verdict")
    if has_regression:
        reg_str = ", ".join(regressions_list)
        lines.append(f"> [!WARNING]")
        lines.append(f"> 🔴 **POTENTIAL REGRESSION DETECTED**: Workloads [{reg_str}] exceeded the {threshold:.1f}% threshold.")
        lines.append("> Please inspect changes before merging.")
    else:
        lines.append(f"> [!TIP]")
        lines.append("> 🟢 **PASS — No Performance Regressions Detected.**")
        lines.append(f"> All metrics across persistence modes are within the {threshold:.1f}% performance threshold.")
    lines.append("")
    return "\n".join(lines), has_regression


def main() -> int:
    parser = argparse.ArgumentParser(description="WhiskDB Persistence Benchmark Branch Runner & Comparison")
    parser.add_argument("--target", help="Target branch/git-ref to compare against (e.g. master, dev)")
    parser.add_argument("--base", default="HEAD", help="PR / Candidate git-ref (default: HEAD)")
    parser.add_argument("--quick", action="store_true", default=True, help="Run quick benchmark profile (default: True)")
    parser.add_argument("--full", action="store_true", help="Run full benchmark profile (more ops)")
    parser.add_argument("--mode", default="all", help="Persistence mode: strict|group_commit|relaxed|memory_only|all")
    parser.add_argument("--ops", type=int, default=0, help="Override ops per workload")
    parser.add_argument("--threads", type=int, default=4, help="Threads for concurrent workloads")
    parser.add_argument("--threshold", type=float, default=10.0, help="Regression threshold percentage (default: 10.0)")
    parser.add_argument("--output-json", help="Path to write output comparison JSON")
    parser.add_argument("--output-markdown", help="Path to write GitHub Markdown report")
    parser.add_argument("--fail-on-regression", action="store_true", help="Exit with non-zero if regression detected")
    args = parser.parse_args()

    quick = not args.full

    repo_root = run_cmd(["git", "rev-parse", "--show-toplevel"], capture=True).stdout.strip()
    os.chdir(repo_root)

    base_sha = resolve_git_ref(args.base, repo_root) or "HEAD"
    base_info = get_git_commit_info(base_sha, repo_root)
    base_name = args.base

    print(f"===========================================================")
    print(f"WhiskDB Benchmark Branch Runner")
    print(f"Candidate (Current): {base_name} [{base_info}]")
    if args.target:
        print(f"Target (Baseline):   {args.target}")
    print(f"===========================================================")

    # 1. Build and run current branch
    base_build_dir = os.path.join(repo_root, "build", "release")
    base_bench_bin = build_bench_executable(repo_root, base_build_dir, repo_root)

    with tempfile.NamedTemporaryFile(suffix="_base.json", delete=False) as tf:
        base_json_path = tf.name

    base_data = run_benchmark(
        base_bench_bin,
        base_json_path,
        quick=quick,
        mode=args.mode,
        ops=args.ops,
        threads=args.threads,
    )

    target_data: Optional[Dict[str, Any]] = None
    target_info = "N/A"
    target_name = args.target or ""

    if args.target:
        target_sha = resolve_git_ref(args.target, repo_root)
        if not target_sha:
            print(f"Warning: Could not resolve git ref '{args.target}'. Treating as non-existent.")
            target_sha = None

        if target_sha:
            target_info = get_git_commit_info(target_sha, repo_root)
            worktree_dir = tempfile.mkdtemp(prefix="whisk_wt_")
            try:
                print(f"==> Setting up clean worktree for '{args.target}' at {worktree_dir}...")
                run_cmd(["git", "worktree", "add", "--detach", worktree_dir, target_sha], cwd=repo_root)

                # Check if target branch has engine code
                target_engine = os.path.join(worktree_dir, "src", "engine.cpp")
                target_bench_src = os.path.join(worktree_dir, "app", "whisk_bench.cpp")

                if not os.path.isfile(target_engine):
                    print(f"Notice: Target branch '{args.target}' lacks database engine src/engine.cpp.")
                else:
                    if not os.path.isfile(target_bench_src):
                        print(f"Notice: Injecting app/whisk_bench.cpp into target branch worktree...")
                        os.makedirs(os.path.join(worktree_dir, "app"), exist_ok=True)
                        shutil.copyfile(
                            os.path.join(repo_root, "app", "whisk_bench.cpp"),
                            target_bench_src,
                        )

                    target_build_dir = os.path.join(worktree_dir, "build", "release")
                    target_bench_bin = build_bench_executable(worktree_dir, target_build_dir, repo_root)

                    with tempfile.NamedTemporaryFile(suffix="_target.json", delete=False) as t_tf:
                        target_json_path = t_tf.name

                    target_data = run_benchmark(
                        target_bench_bin,
                        target_json_path,
                        quick=quick,
                        mode=args.mode,
                        ops=args.ops,
                        threads=args.threads,
                    )
            finally:
                print(f"==> Cleaning up worktree at {worktree_dir}...")
                run_cmd(["git", "worktree", "remove", "--force", worktree_dir], cwd=repo_root, check=False)
                if os.path.exists(worktree_dir):
                    shutil.rmtree(worktree_dir, ignore_errors=True)

    # Generate Markdown Report
    md_content, has_regression = generate_markdown_report(
        target_name=target_name,
        target_info=target_info,
        base_name=base_name,
        base_info=base_info,
        target_data=target_data,
        base_data=base_data,
        threshold=args.threshold,
    )

    if args.output_markdown:
        with open(args.output_markdown, "w") as f:
            f.write(md_content)
        print(f"Saved Markdown report to: {args.output_markdown}")

    comparison_json = {
        "candidate": {"ref": base_name, "info": base_info, "data": base_data},
        "target": {"ref": target_name, "info": target_info, "data": target_data},
        "has_regression": has_regression,
        "threshold": args.threshold,
    }

    if args.output_json:
        with open(args.output_json, "w") as f:
            json.dump(comparison_json, f, indent=2)
        print(f"Saved Comparison JSON to: {args.output_json}")

    print("\n" + md_content + "\n")

    if has_regression and args.fail_on_regression:
        print("ERROR: Performance regression detected above threshold!")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
