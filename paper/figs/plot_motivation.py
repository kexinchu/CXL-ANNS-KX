#!/usr/bin/env python3
"""Draw the measured Motivation latency and admission figures."""
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
    # CXL-DRAM hit = dax0.0 clflush+load; Flash fill = vmem0 cache miss.
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
        label="CXL-DRAM hit",
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
        label="Flash fill",
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
    fig4_rc = {
        "font.size": 12,
        "axes.labelsize": 12,
        "axes.titlesize": 12,
        "xtick.labelsize": 12,
        "ytick.labelsize": 12,
        "legend.fontsize": 12,
    }
    figsize = (6.4 * 0.8, 4.8 * 0.6)

    with mpl.rc_context(fig4_rc):
        # (a) Record-granular vs. 16-KB page admission — F2.
        fig, ax = plt.subplots(figsize=figsize, layout="constrained")
        categories = ["Record", "Page 16 KB"]
        qps = np.array([33.5, 9.3])
        nand_k = np.array([9.858, 36.493])
        x = np.arange(len(categories))
        width = 0.34
        qps_bars = ax.bar(
            x - width / 2,
            qps,
            width=width,
            color=C_DARK,
            edgecolor="black",
            linewidth=0.8,
            label="QPS",
            zorder=3,
        )
        ax2 = ax.twinx()
        nand_bars = ax2.bar(
            x + width / 2,
            nand_k,
            width=width,
            color=C_LIGHT,
            edgecolor="black",
            linewidth=0.8,
            hatch="//",
            label="NAND reads",
            zorder=3,
        )
        ax.set_xticks(x)
        ax.set_xticklabels(categories)
        ax.set_ylabel("Throughput (QPS)")
        ax2.set_ylabel("NAND reads (thousands)")
        ax.set_ylim(0, 44)
        ax2.set_ylim(0, 48)
        ax.set_title("(a) Coarse admission wastes fills")
        ax.bar_label(qps_bars, labels=["33.5", "9.3"], padding=3)
        ax2.bar_label(nand_bars, labels=["9.9", "36.5"], padding=3)
        ax.text(
            0.50,
            0.76,
            "3.6× lower QPS\n3.7× more NAND",
            transform=ax.transAxes,
            ha="center",
            va="center",
        )
        h1, l1 = ax.get_legend_handles_labels()
        h2, l2 = ax2.get_legend_handles_labels()
        ax.legend(
            h1 + h2,
            l1 + l2,
            loc="upper center",
            bbox_to_anchor=(0.5, 1.01),
            ncol=2,
            frameon=False,
            handlelength=1.2,
        )
        style_ax(ax)
        ax2.spines["top"].set_visible(False)
        ax2.tick_params(direction="out", length=3)
        admission_out = OUT / "mot-pathology-admission.pdf"
        fig.savefig(
            admission_out,
            format="pdf",
            metadata={"Creator": "matplotlib vector"},
        )
        plt.close(fig)

        # (b) Demand vs. increasingly broad synchronous prefetch — F5.
        fig, ax = plt.subplots(figsize=figsize, layout="constrained")
        names = ["Demand", "Top-1", "Top-2", "Top-8"]
        qps = np.array([25.9, 21.0, 15.2, 5.0])
        precision = ["No prefetch", "82%", "68%", "29%"]
        x = np.arange(len(names))
        bars = ax.bar(
            x,
            qps,
            width=0.62,
            color=[C_LIGHT, "#b5b5b5", "#969696", C_DARK],
            edgecolor="black",
            linewidth=0.8,
            zorder=3,
        )
        ax.set_xticks(x)
        ax.set_xticklabels(names)
        ax.set_ylabel("Throughput (QPS)")
        ax.set_ylim(0, 35)
        ax.set_title("(b) Wider prefetch fetches wrong pages")
        for bar, value, prec in zip(bars, qps, precision):
            ax.text(
                bar.get_x() + bar.get_width() / 2,
                value + 0.7,
                f"{value:.1f}\n{prec}",
                ha="center",
                va="bottom",
            )
        ax.set_xlabel("Prefetch coverage increases $\\longrightarrow$")
        style_ax(ax)
        prefetch_out = OUT / "mot-pathology-prefetch.pdf"
        fig.savefig(
            prefetch_out,
            format="pdf",
            metadata={"Creator": "matplotlib vector"},
        )
        plt.close(fig)

    print("wrote", admission_out)
    print("wrote", prefetch_out)


if __name__ == "__main__":
    fig3_cliff()
    fig4_pathology()
