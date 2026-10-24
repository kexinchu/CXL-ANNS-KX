#!/usr/bin/env python3
"""Option A: QD_eff vs concurrent queries. Broken y-axis so the T-rise
and the measured QDsat line are both readable."""
import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator


def load_summary(run: Path):
    rows = []
    for line in (run / "summary.jsonl").read_text().splitlines():
        if line.strip():
            rows.append(json.loads(line))
    by_tag = {r["tag"]: r for r in rows}
    out = []
    for t, tag in [(1, "T1"), (2, "T2"), (4, "T4"), (8, "T8"), (16, "T16")]:
        if tag in by_tag:
            r = dict(by_tag[tag])
            r["T"] = t
            out.append(r)
    return out


def broken_marks(ax_top, ax_bot):
    d = 0.012
    kwargs = dict(transform=ax_top.transAxes, color="black", clip_on=False, linewidth=0.7)
    ax_top.plot((-d, +d), (-d, +d), **kwargs)
    ax_top.plot((1 - d, 1 + d), (-d, +d), **kwargs)
    kwargs["transform"] = ax_bot.transAxes
    ax_bot.plot((-d, +d), (1 - d, 1 + d), **kwargs)
    ax_bot.plot((1 - d, 1 + d), (1 - d, 1 + d), **kwargs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run", type=Path)
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()
    rows = load_summary(args.run)
    if not rows:
        raise SystemExit(f"no T rows in {args.run}/summary.jsonl")
    xs = [r["T"] for r in rows]
    ys = [r.get("QD_eff") for r in rows]
    if any(y is None for y in ys):
        raise SystemExit("QD_eff missing; refuse to plot empty/None points")
    qdsat = rows[0].get("QDsat")
    recs = [r.get("recall") for r in rows]
    rec1 = recs[0]
    iso = all(r is not None and rec1 is not None and abs(r - rec1) <= 0.005 for r in recs)

    fig, (ax_top, ax_bot) = plt.subplots(
        2, 1, sharex=True, figsize=(3.35, 2.35),
        gridspec_kw={"height_ratios": [0.22, 0.78], "hspace": 0.06},
    )
    ymax = max(ys) * 1.35
    ax_bot.plot(xs, ys, color="black", linewidth=0.9, zorder=1)
    ax_bot.scatter(xs, ys, s=26, c="black", zorder=2)
    for i, (x, y) in enumerate(zip(xs, ys)):
        dx, ha = (-6, "right") if i == len(xs) - 1 else (5, "left")
        ax_bot.annotate(f"{y:.2f}", (x, y), textcoords="offset points",
                        xytext=(dx, 5), fontsize=7, ha=ha)
    ax_bot.set_ylim(0, ymax)
    ax_bot.yaxis.set_major_locator(MaxNLocator(4))
    ax_bot.set_ylabel("Effective queue depth", fontsize=8)
    ax_bot.set_xlabel("Concurrent queries", fontsize=8)
    ax_bot.set_xticks(xs)
    ax_bot.tick_params(labelsize=8)
    ax_bot.spines["top"].set_visible(False)

    if qdsat is not None:
        pad = max(40.0, 0.04 * qdsat)
        ax_top.axhline(qdsat, color="black", linestyle="--", linewidth=0.8)
        ax_top.set_ylim(qdsat - pad, qdsat + pad)
        ax_top.set_yticks([qdsat])
        ax_top.set_yticklabels([f"{qdsat:.0f}"])
        ax_top.text(0.02, 0.55, r"$QD_{\mathrm{sat}}$", transform=ax_top.transAxes,
                    va="center", ha="left", fontsize=8)
        ax_top.tick_params(labelsize=8, bottom=False, labelbottom=False)
        ax_top.spines["bottom"].set_visible(False)
        ax_top.spines["top"].set_visible(False)
        ax_top.spines["right"].set_visible(False)
        broken_marks(ax_top, ax_bot)
    else:
        ax_top.remove()

    ax_bot.spines["right"].set_visible(False)
    fig.align_ylabels([ax_bot])
    fig.subplots_adjust(left=0.20, right=0.97, top=0.96, bottom=0.18)

    out = args.out or Path("paper/figs/mot-c3-io-concurrency.pdf")
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, format="pdf")
    fig.savefig(out.with_suffix(".png"), format="png", dpi=200)
    print(f"wrote {out} QD_eff={ys} QDsat={qdsat} recall={recs} iso={iso}")


if __name__ == "__main__":
    main()
