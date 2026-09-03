#!/usr/bin/env python3
"""Draw Motivation Fig.3 and Fig.4 from locked LAION-200k findings."""
from __future__ import annotations

from pathlib import Path

import matplotlib as mpl
import matplotlib.pyplot as plt
import numpy as np

OUT = Path(__file__).resolve().parent
mpl.rcParams.update(
    {
        "font.family": "serif",
        "font.size": 8,
        "axes.labelsize": 8,
        "axes.titlesize": 8,
        "xtick.labelsize": 7.5,
        "ytick.labelsize": 7.5,
        "legend.fontsize": 7,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
        "pdf.use14corefonts": False,
        "text.usetex": False,
        "axes.linewidth": 0.7,
        "xtick.major.width": 0.6,
        "ytick.major.width": 0.6,
    }
)

C_DARK = "#7a7a7a"
C_MID = "#5a5a5a"
C_LIGHT = "#d4d4d4"
C_ACCENT = "#2c5aa0"
C_WARM = "#b85c38"


def style_ax(ax):
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.tick_params(direction="out", length=3)


def fig3_cliff():
    # p50 ns/item from probe_item_cliff 2026-09-02b.
    # CXL-DRAM = dax0.0 clflush+load; CXL-SSD = vmem0 cache miss.
    datasets = ["LAION-10M", "T2I-10M"]
    dram_us = np.array([0.666, 0.281])
    ssd_us = np.array([88.713, 88.841])
    x = np.arange(len(datasets))
    w = 0.36

    fig, ax = plt.subplots(figsize=(6.4*0.8, 4.8*0.6))
    ax.tick_params(labelsize=12)
    ax.xaxis.label.set_size(12)
    ax.yaxis.label.set_size(12)
    b0 = ax.bar(
        x - w / 2,
        dram_us,
        width=w,
        color=C_LIGHT,
        edgecolor="black",
        linewidth=0.8,
        label="CXL-DRAM",
        zorder=3,
        rasterized=False,
    )
    b1 = ax.bar(
        x + w / 2,
        ssd_us,
        width=w,
        color=C_DARK,
        edgecolor="black",
        linewidth=0.8,
        label="CXL-SSD",
        zorder=3,
        rasterized=False,
    )
    ax.set_yscale("log")
    ax.set_xticks(x)
    ax.set_xticklabels(datasets)
    ax.set_ylabel(r"Latency ($\mu$s / item)")
    ax.set_ylim(0.08, 400)
    ax.yaxis.set_major_formatter(mpl.ticker.FuncFormatter(lambda v, _p: f"{v:g}"))
    ax.legend(frameon=False, loc="upper left", fontsize=12, ncol=2, bbox_to_anchor=(0.1, 1.1))
    for bar, val in zip(b0, dram_us):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            val * 1.25,
            f"{val:.2f}",
            ha="center",
            va="bottom",
            fontsize=11,
        )
    for bar, val in zip(b1, ssd_us):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            val * 1.12,
            f"{val:.2f}",
            ha="center",
            va="bottom",
            fontsize=11,
        )
    style_ax(ax)
    fig.tight_layout(pad=0.35)
    fig.savefig(
        OUT / "mot-cliff.pdf",
        format="pdf",
        bbox_inches="tight",
        pad_inches=0.03,
        dpi=600,
        metadata={"Creator": "matplotlib vector"},
    )
    plt.close(fig)
    print("wrote", OUT / "mot-cliff.pdf")


def fig4_pathology():
    fig, axes = plt.subplots(1, 3, figsize=(7.05, 2.35))

    # (a) record vs page16k — F2
    ax = axes[0]
    cats = ["record", "page 16 KB"]
    qps = np.array([33.5, 9.3])
    nand = np.array([9858, 36493])
    x = np.arange(2)
    w = 0.36
    ax.bar(x - w / 2, qps, width=w, color=C_ACCENT, edgecolor="black", linewidth=0.4, label="QPS")
    ax2 = ax.twinx()
    ax2.bar(x + w / 2, nand / 1000.0, width=w, color=C_WARM, edgecolor="black", linewidth=0.4, label="NAND")
    ax.set_xticks(x)
    ax.set_xticklabels(cats)
    ax.set_ylabel("QPS")
    ax2.set_ylabel("NAND reads ($10^3$)")
    ax.set_title("(a) Page vs. record")
    ax.set_ylim(0, 42)
    ax2.set_ylim(0, 48)
    ax.text(0, 35.2, "33.5", ha="center", fontsize=6.5, color=C_ACCENT)
    ax.text(1, 11.0, "9.3", ha="center", fontsize=6.5, color=C_ACCENT)
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, loc="upper right", frameon=False, handlelength=1.0)
    style_ax(ax)
    ax2.spines["top"].set_visible(False)
    ax2.tick_params(direction="out", length=3)

    # (b) demand vs sync 2-hop — F5
    ax = axes[1]
    names = ["demand", "top-1", "top-2", "top-8"]
    qps_b = np.array([25.9, 21.0, 15.2, 5.0])
    hit = np.array([57.51, 57.51, 57.51, 57.51])
    prec = np.array([np.nan, 82.35, 68.45, 29.02])
    x = np.arange(4)
    ax.bar(x, qps_b, width=0.62, color=C_ACCENT, edgecolor="black", linewidth=0.4)
    ax.set_xticks(x)
    ax.set_xticklabels(names, rotation=15)
    ax.set_ylabel("QPS")
    ax.set_ylim(0, 36)
    ax.set_title("(b) Blind sync prefetch")
    for i, (q, h, p) in enumerate(zip(qps_b, hit, prec)):
        label = f"hit {h:.0f}%"
        if np.isfinite(p):
            label += f"\nprec {p:.0f}%"
        ax.text(i, q + 0.7, label, ha="center", va="bottom", fontsize=5.6, color=C_DARK)
    style_ax(ax)

    # (c) beam sweep — F4
    ax = axes[2]
    beam = np.array([4, 8, 16, 32, 64])
    qps_c = np.array([35.93, 27.17, 27.18, 23.94, 27.32])
    hit_c = np.array([62.59, 60.98, 61.43, 61.01, 61.01])
    rec = np.array([0.0938, 0.1875, 0.1750, 0.2438, 0.2438])
    nand_c = np.array([4763, 6286, 6226, 6294, 6294])
    ax.plot(beam, qps_c / qps_c.max(), "-o", color=C_ACCENT, ms=3.5, lw=1.1, label="QPS")
    ax.plot(beam, nand_c / nand_c.max(), "-s", color=C_WARM, ms=3.2, lw=1.1, label="NAND")
    ax.plot(beam, hit_c / 100.0, ":D", color=C_MID, ms=3.0, lw=1.0, label="hit")
    ax2 = ax.twinx()
    ax2.plot(beam, rec, "--^", color=C_DARK, ms=3.5, lw=1.1, label="recall@10")
    ax.set_xscale("log", base=2)
    ax.set_xticks(beam)
    ax.xaxis.set_major_formatter(mpl.ticker.FormatStrFormatter("%d"))
    ax.set_xlabel("beam $L$")
    ax.set_ylabel("normalized QPS / NAND / hit")
    ax2.set_ylabel("recall@10")
    ax2.set_ylim(0, 0.40)
    ax.set_ylim(0, 1.15)
    ax.set_title("(c) Widening $L$")
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(
        h1 + h2,
        l1 + l2,
        loc="center right",
        frameon=False,
        ncol=1,
        handlelength=1.3,
        borderaxespad=0.2,
    )
    style_ax(ax)
    ax2.spines["top"].set_visible(False)
    ax2.tick_params(direction="out", length=3)

    fig.tight_layout(pad=0.35, w_pad=0.7)
    fig.savefig(OUT / "mot-pathology.pdf", bbox_inches="tight", pad_inches=0.03)
    plt.close(fig)
    print("wrote", OUT / "mot-pathology.pdf")


if __name__ == "__main__":
    fig3_cliff()
    fig4_pathology()
