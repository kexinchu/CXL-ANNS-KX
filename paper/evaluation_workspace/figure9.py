#!/usr/bin/env python3
"""Standalone, evidence-embedded renderer for paper Figure 9.

This file performs no runtime reads from experiment results.  The 33 plotted
aggregate marks and their 165 supporting accepted run IDs are embedded below.
"""

from __future__ import annotations

import math
from pathlib import Path
from typing import Iterable

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


SOURCE_AGGREGATE_SHA256 = "aae1726b74e1b606273998c51397478f372a4ecbd9d261b02c277ddc9016af61"
FIGURE9_ROWS = [
  {
    "dataset": "t2i10m",
    "phase": "q3_t1",
    "system": "batch-t1",
    "run_ids": [
      "t2i10m-q3_t1-L400-r0-cold-batch-t1-4ad7-c3ef",
      "t2i10m-q3_t1-L400-r1-cold-batch-t1-4ad7-c3ef",
      "t2i10m-q3_t1-L400-r2-cold-batch-t1-4ad7-c3ef",
      "t2i10m-q3_t1-L400-r3-cold-batch-t1-4ad7-c3ef",
      "t2i10m-q3_t1-L400-r4-cold-batch-t1-4ad7-c3ef"
    ],
    "qps_median": 90.34,
    "qps_ci_low": 90.29,
    "qps_ci_high": 91.39,
    "threads": 1,
    "committed_candidates_median": 400,
    "missing_pages_median": 355.4418,
    "issued_pages_per_query_median": 355.4418,
    "nand_read_bytes_median": 9830072320,
    "nq_median": 10000,
    "nand_commands_per_query_median": 237.321
  },
  {
    "dataset": "t2i10m",
    "phase": "q3_t1",
    "system": "extent-t1",
    "run_ids": [
      "t2i10m-q3_t1-L400-r0-cold-extent-t1-4ad7-c3ef",
      "t2i10m-q3_t1-L400-r1-cold-extent-t1-4ad7-c3ef",
      "t2i10m-q3_t1-L400-r2-cold-extent-t1-4ad7-c3ef",
      "t2i10m-q3_t1-L400-r3-cold-extent-t1-4ad7-c3ef",
      "t2i10m-q3_t1-L400-r4-cold-extent-t1-4ad7-c3ef"
    ],
    "qps_median": 91.61,
    "qps_ci_low": 90.37,
    "qps_ci_high": 92.83,
    "threads": 1,
    "committed_candidates_median": 400,
    "missing_pages_median": 355.4462,
    "issued_pages_per_query_median": 355.4463,
    "nand_read_bytes_median": 9830105088,
    "nq_median": 10000,
    "nand_commands_per_query_median": 220.6605
  },
  {
    "dataset": "t2i10m",
    "phase": "q3_t1",
    "system": "serial-t1",
    "run_ids": [
      "t2i10m-q3_t1-L400-r0-cold-serial-t1-4ad7-c3ef",
      "t2i10m-q3_t1-L400-r1-cold-serial-t1-4ad7-c3ef",
      "t2i10m-q3_t1-L400-r2-cold-serial-t1-4ad7-c3ef",
      "t2i10m-q3_t1-L400-r3-cold-serial-t1-4ad7-c3ef",
      "t2i10m-q3_t1-L400-r4-cold-serial-t1-4ad7-c3ef"
    ],
    "qps_median": 35.27,
    "qps_ci_low": 35.15,
    "qps_ci_high": 35.44,
    "threads": 1,
    "committed_candidates_median": 400,
    "missing_pages_median": 355.4418,
    "issued_pages_per_query_median": 355.4418,
    "nand_read_bytes_median": 10085208064,
    "nq_median": 10000,
    "nand_commands_per_query_median": 246.2209
  },
  {
    "dataset": "laion10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "laion10m-q3_t8-L400-r0-cold-flashanns-T1-4ad7-c3ef",
      "laion10m-q3_t8-L400-r1-cold-flashanns-T1-4ad7-c3ef",
      "laion10m-q3_t8-L400-r2-cold-flashanns-T1-4ad7-c3ef",
      "laion10m-q3_t8-L400-r3-cold-flashanns-T1-4ad7-c3ef",
      "laion10m-q3_t8-L400-r4-cold-flashanns-T1-4ad7-c3ef"
    ],
    "qps_median": 65.53,
    "qps_ci_low": 63.67,
    "qps_ci_high": 66.93,
    "threads": 1,
    "committed_candidates_median": 400,
    "missing_pages_median": 389.5819,
    "issued_pages_per_query_median": 389.5819,
    "nand_read_bytes_median": 9524871168,
    "nq_median": 10000,
    "nand_commands_per_query_median": 209.1933
  },
  {
    "dataset": "laion10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "laion10m-q3_t8-L400-r0-cold-flashanns-T2-4ad7-c3ef",
      "laion10m-q3_t8-L400-r1-cold-flashanns-T2-4ad7-c3ef",
      "laion10m-q3_t8-L400-r2-cold-flashanns-T2-4ad7-c3ef",
      "laion10m-q3_t8-L400-r3-cold-flashanns-T2-4ad7-c3ef",
      "laion10m-q3_t8-L400-r4-cold-flashanns-T2-4ad7-c3ef"
    ],
    "qps_median": 128.48,
    "qps_ci_low": 125.68,
    "qps_ci_high": 129.49,
    "threads": 2,
    "committed_candidates_median": 400,
    "missing_pages_median": 389.6704,
    "issued_pages_per_query_median": 389.6704,
    "nand_read_bytes_median": 9541156864,
    "nq_median": 10000,
    "nand_commands_per_query_median": 209.5621
  },
  {
    "dataset": "laion10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "laion10m-q3_t8-L400-r0-cold-flashanns-T4-4ad7-c3ef",
      "laion10m-q3_t8-L400-r1-cold-flashanns-T4-4ad7-c3ef",
      "laion10m-q3_t8-L400-r2-cold-flashanns-T4-4ad7-c3ef",
      "laion10m-q3_t8-L400-r3-cold-flashanns-T4-4ad7-c3ef",
      "laion10m-q3_t8-L400-r4-cold-flashanns-T4-4ad7-c3ef"
    ],
    "qps_median": 239.26,
    "qps_ci_low": 235.69,
    "qps_ci_high": 243.8,
    "threads": 4,
    "committed_candidates_median": 400,
    "missing_pages_median": 389.8784,
    "issued_pages_per_query_median": 389.8785,
    "nand_read_bytes_median": 9546027008,
    "nq_median": 10000,
    "nand_commands_per_query_median": 209.6798
  },
  {
    "dataset": "laion10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "laion10m-q3_t8-L400-r0-cold-flashanns-T8-4ad7-c3ef",
      "laion10m-q3_t8-L400-r1-cold-flashanns-T8-4ad7-c3ef",
      "laion10m-q3_t8-L400-r2-cold-flashanns-T8-4ad7-c3ef",
      "laion10m-q3_t8-L400-r3-cold-flashanns-T8-4ad7-c3ef",
      "laion10m-q3_t8-L400-r4-cold-flashanns-T8-4ad7-c3ef"
    ],
    "qps_median": 421.24,
    "qps_ci_low": 419.34,
    "qps_ci_high": 424.69,
    "threads": 8,
    "committed_candidates_median": 400,
    "missing_pages_median": 390.2993,
    "issued_pages_per_query_median": 390.2993,
    "nand_read_bytes_median": 9553084416,
    "nq_median": 10000,
    "nand_commands_per_query_median": 209.8516
  },
  {
    "dataset": "laion10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "laion10m-q3_t8-L400-r0-cold-flashanns-T16-4ad7-c3ef",
      "laion10m-q3_t8-L400-r1-cold-flashanns-T16-4ad7-c3ef",
      "laion10m-q3_t8-L400-r2-cold-flashanns-T16-4ad7-c3ef",
      "laion10m-q3_t8-L400-r3-cold-flashanns-T16-4ad7-c3ef",
      "laion10m-q3_t8-L400-r4-cold-flashanns-T16-4ad7-c3ef"
    ],
    "qps_median": 782.27,
    "qps_ci_low": 753.01,
    "qps_ci_high": 791.05,
    "threads": 16,
    "committed_candidates_median": 400,
    "missing_pages_median": 391.0292,
    "issued_pages_per_query_median": 391.0292,
    "nand_read_bytes_median": 9562181632,
    "nq_median": 10000,
    "nand_commands_per_query_median": 210.0475
  },
  {
    "dataset": "laion10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "laion10m-q3_t8-L400-r0-cold-wise-only-T1-4ad7-c3ef",
      "laion10m-q3_t8-L400-r1-cold-wise-only-T1-4ad7-c3ef",
      "laion10m-q3_t8-L400-r2-cold-wise-only-T1-4ad7-c3ef",
      "laion10m-q3_t8-L400-r3-cold-wise-only-T1-4ad7-c3ef",
      "laion10m-q3_t8-L400-r4-cold-wise-only-T1-4ad7-c3ef"
    ],
    "qps_median": 66.3,
    "qps_ci_low": 65.36,
    "qps_ci_high": 66.88,
    "threads": 1,
    "committed_candidates_median": 400,
    "missing_pages_median": 389.582,
    "issued_pages_per_query_median": 389.582,
    "nand_read_bytes_median": 9524871168,
    "nq_median": 10000,
    "nand_commands_per_query_median": 209.1933
  },
  {
    "dataset": "laion10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "laion10m-q3_t8-L400-r0-cold-wise-only-T2-4ad7-c3ef",
      "laion10m-q3_t8-L400-r1-cold-wise-only-T2-4ad7-c3ef",
      "laion10m-q3_t8-L400-r2-cold-wise-only-T2-4ad7-c3ef",
      "laion10m-q3_t8-L400-r3-cold-wise-only-T2-4ad7-c3ef",
      "laion10m-q3_t8-L400-r4-cold-wise-only-T2-4ad7-c3ef"
    ],
    "qps_median": 127.72,
    "qps_ci_low": 125.35,
    "qps_ci_high": 129.39,
    "threads": 2,
    "committed_candidates_median": 400,
    "missing_pages_median": 389.6643,
    "issued_pages_per_query_median": 389.6648,
    "nand_read_bytes_median": 9539690496,
    "nq_median": 10000,
    "nand_commands_per_query_median": 209.5341
  },
  {
    "dataset": "laion10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "laion10m-q3_t8-L400-r0-cold-wise-only-T4-4ad7-c3ef",
      "laion10m-q3_t8-L400-r1-cold-wise-only-T4-4ad7-c3ef",
      "laion10m-q3_t8-L400-r2-cold-wise-only-T4-4ad7-c3ef",
      "laion10m-q3_t8-L400-r3-cold-wise-only-T4-4ad7-c3ef",
      "laion10m-q3_t8-L400-r4-cold-wise-only-T4-4ad7-c3ef"
    ],
    "qps_median": 236.69,
    "qps_ci_low": 235.02,
    "qps_ci_high": 241.77,
    "threads": 4,
    "committed_candidates_median": 400,
    "missing_pages_median": 389.6759,
    "issued_pages_per_query_median": 389.6761,
    "nand_read_bytes_median": 9545297920,
    "nq_median": 10000,
    "nand_commands_per_query_median": 209.6768
  },
  {
    "dataset": "laion10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "laion10m-q3_t8-L400-r0-cold-wise-only-T8-4ad7-c3ef",
      "laion10m-q3_t8-L400-r1-cold-wise-only-T8-4ad7-c3ef",
      "laion10m-q3_t8-L400-r2-cold-wise-only-T8-4ad7-c3ef",
      "laion10m-q3_t8-L400-r3-cold-wise-only-T8-4ad7-c3ef",
      "laion10m-q3_t8-L400-r4-cold-wise-only-T8-4ad7-c3ef"
    ],
    "qps_median": 406.01,
    "qps_ci_low": 400.03,
    "qps_ci_high": 419.06,
    "threads": 8,
    "committed_candidates_median": 400,
    "missing_pages_median": 390.5729,
    "issued_pages_per_query_median": 390.5732,
    "nand_read_bytes_median": 9553768448,
    "nq_median": 10000,
    "nand_commands_per_query_median": 209.8491
  },
  {
    "dataset": "laion10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "laion10m-q3_t8-L400-r0-cold-wise-only-T16-4ad7-c3ef",
      "laion10m-q3_t8-L400-r1-cold-wise-only-T16-4ad7-c3ef",
      "laion10m-q3_t8-L400-r2-cold-wise-only-T16-4ad7-c3ef",
      "laion10m-q3_t8-L400-r3-cold-wise-only-T16-4ad7-c3ef",
      "laion10m-q3_t8-L400-r4-cold-wise-only-T16-4ad7-c3ef"
    ],
    "qps_median": 702.46,
    "qps_ci_low": 673.68,
    "qps_ci_high": 711.95,
    "threads": 16,
    "committed_candidates_median": 400,
    "missing_pages_median": 391.1685,
    "issued_pages_per_query_median": 391.169,
    "nand_read_bytes_median": 9565749248,
    "nq_median": 10000,
    "nand_commands_per_query_median": 210.1484
  },
  {
    "dataset": "t2i10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "t2i10m-q3_t8-L400-r0-cold-flashanns-T1-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r1-cold-flashanns-T1-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r2-cold-flashanns-T1-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r3-cold-flashanns-T1-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r4-cold-flashanns-T1-4ad7-c3ef"
    ],
    "qps_median": 92.5,
    "qps_ci_low": 92,
    "qps_ci_high": 93.14,
    "threads": 1,
    "committed_candidates_median": 400,
    "missing_pages_median": 355.4453,
    "issued_pages_per_query_median": 355.4454,
    "nand_read_bytes_median": 9830117376,
    "nq_median": 10000,
    "nand_commands_per_query_median": 220.661
  },
  {
    "dataset": "t2i10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "t2i10m-q3_t8-L400-r0-cold-flashanns-T2-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r1-cold-flashanns-T2-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r2-cold-flashanns-T2-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r3-cold-flashanns-T2-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r4-cold-flashanns-T2-4ad7-c3ef"
    ],
    "qps_median": 176.48,
    "qps_ci_low": 174.43,
    "qps_ci_high": 178.94,
    "threads": 2,
    "committed_candidates_median": 400,
    "missing_pages_median": 355.6088,
    "issued_pages_per_query_median": 355.6088,
    "nand_read_bytes_median": 9843347456,
    "nq_median": 10000,
    "nand_commands_per_query_median": 220.9587
  },
  {
    "dataset": "t2i10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "t2i10m-q3_t8-L400-r0-cold-flashanns-T4-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r1-cold-flashanns-T4-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r2-cold-flashanns-T4-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r3-cold-flashanns-T4-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r4-cold-flashanns-T4-4ad7-c3ef"
    ],
    "qps_median": 321.45,
    "qps_ci_low": 315.87,
    "qps_ci_high": 326.01,
    "threads": 4,
    "committed_candidates_median": 400,
    "missing_pages_median": 355.9173,
    "issued_pages_per_query_median": 355.9176,
    "nand_read_bytes_median": 9848537088,
    "nq_median": 10000,
    "nand_commands_per_query_median": 221.0775
  },
  {
    "dataset": "t2i10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "t2i10m-q3_t8-L400-r0-cold-flashanns-T8-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r1-cold-flashanns-T8-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r2-cold-flashanns-T8-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r3-cold-flashanns-T8-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r4-cold-flashanns-T8-4ad7-c3ef"
    ],
    "qps_median": 600.78,
    "qps_ci_low": 595.71,
    "qps_ci_high": 605.51,
    "threads": 8,
    "committed_candidates_median": 400,
    "missing_pages_median": 356.2234,
    "issued_pages_per_query_median": 356.2234,
    "nand_read_bytes_median": 9856884736,
    "nq_median": 10000,
    "nand_commands_per_query_median": 221.2586
  },
  {
    "dataset": "t2i10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "t2i10m-q3_t8-L400-r0-cold-flashanns-T16-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r1-cold-flashanns-T16-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r2-cold-flashanns-T16-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r3-cold-flashanns-T16-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r4-cold-flashanns-T16-4ad7-c3ef"
    ],
    "qps_median": 923.34,
    "qps_ci_low": 896.71,
    "qps_ci_high": 934.08,
    "threads": 16,
    "committed_candidates_median": 400,
    "missing_pages_median": 356.6908,
    "issued_pages_per_query_median": 356.6908,
    "nand_read_bytes_median": 9866309632,
    "nq_median": 10000,
    "nand_commands_per_query_median": 221.4891
  },
  {
    "dataset": "t2i10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "t2i10m-q3_t8-L400-r0-cold-wise-only-T1-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r1-cold-wise-only-T1-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r2-cold-wise-only-T1-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r3-cold-wise-only-T1-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r4-cold-wise-only-T1-4ad7-c3ef"
    ],
    "qps_median": 90.48,
    "qps_ci_low": 88.8,
    "qps_ci_high": 92.74,
    "threads": 1,
    "committed_candidates_median": 400,
    "missing_pages_median": 355.4458,
    "issued_pages_per_query_median": 355.4458,
    "nand_read_bytes_median": 9830096896,
    "nq_median": 10000,
    "nand_commands_per_query_median": 220.6603
  },
  {
    "dataset": "t2i10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "t2i10m-q3_t8-L400-r0-cold-wise-only-T2-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r1-cold-wise-only-T2-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r2-cold-wise-only-T2-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r3-cold-wise-only-T2-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r4-cold-wise-only-T2-4ad7-c3ef"
    ],
    "qps_median": 176.12,
    "qps_ci_low": 175.29,
    "qps_ci_high": 179.9,
    "threads": 2,
    "committed_candidates_median": 400,
    "missing_pages_median": 355.4377,
    "issued_pages_per_query_median": 355.4379,
    "nand_read_bytes_median": 9843175424,
    "nq_median": 10000,
    "nand_commands_per_query_median": 220.9612
  },
  {
    "dataset": "t2i10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "t2i10m-q3_t8-L400-r0-cold-wise-only-T4-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r1-cold-wise-only-T4-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r2-cold-wise-only-T4-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r3-cold-wise-only-T4-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r4-cold-wise-only-T4-4ad7-c3ef"
    ],
    "qps_median": 322.2,
    "qps_ci_low": 315.98,
    "qps_ci_high": 329.78,
    "threads": 4,
    "committed_candidates_median": 400,
    "missing_pages_median": 355.8214,
    "issued_pages_per_query_median": 355.8215,
    "nand_read_bytes_median": 9849929728,
    "nq_median": 10000,
    "nand_commands_per_query_median": 221.0955
  },
  {
    "dataset": "t2i10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "t2i10m-q3_t8-L400-r0-cold-wise-only-T8-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r1-cold-wise-only-T8-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r2-cold-wise-only-T8-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r3-cold-wise-only-T8-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r4-cold-wise-only-T8-4ad7-c3ef"
    ],
    "qps_median": 559.17,
    "qps_ci_low": 550.3,
    "qps_ci_high": 565,
    "threads": 8,
    "committed_candidates_median": 400,
    "missing_pages_median": 356.0081,
    "issued_pages_per_query_median": 356.0082,
    "nand_read_bytes_median": 9856794624,
    "nq_median": 10000,
    "nand_commands_per_query_median": 221.278
  },
  {
    "dataset": "t2i10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "t2i10m-q3_t8-L400-r0-cold-wise-only-T16-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r1-cold-wise-only-T16-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r2-cold-wise-only-T16-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r3-cold-wise-only-T16-4ad7-c3ef",
      "t2i10m-q3_t8-L400-r4-cold-wise-only-T16-4ad7-c3ef"
    ],
    "qps_median": 885.9,
    "qps_ci_low": 865,
    "qps_ci_high": 913.33,
    "threads": 16,
    "committed_candidates_median": 400,
    "missing_pages_median": 356.714,
    "issued_pages_per_query_median": 356.7145,
    "nand_read_bytes_median": 9871245312,
    "nq_median": 10000,
    "nand_commands_per_query_median": 221.6072
  },
  {
    "dataset": "yfcc10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "yfcc10m-q3_t8-L50-r0-cold-flashanns-T1-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r1-cold-flashanns-T1-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r2-cold-flashanns-T1-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r3-cold-flashanns-T1-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r4-cold-flashanns-T1-4ad7-c3ef"
    ],
    "qps_median": 315.76,
    "qps_ci_low": 307.15,
    "qps_ci_high": 320.21,
    "threads": 1,
    "committed_candidates_median": 50,
    "missing_pages_median": 44.4128,
    "issued_pages_per_query_median": 44.4128,
    "nand_read_bytes_median": 1647058944,
    "nq_median": 10000,
    "nand_commands_per_query_median": 39.278
  },
  {
    "dataset": "yfcc10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "yfcc10m-q3_t8-L50-r0-cold-flashanns-T2-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r1-cold-flashanns-T2-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r2-cold-flashanns-T2-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r3-cold-flashanns-T2-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r4-cold-flashanns-T2-4ad7-c3ef"
    ],
    "qps_median": 658.34,
    "qps_ci_low": 647.27,
    "qps_ci_high": 672.49,
    "threads": 2,
    "committed_candidates_median": 50,
    "missing_pages_median": 44.4439,
    "issued_pages_per_query_median": 44.4439,
    "nand_read_bytes_median": 1648082944,
    "nq_median": 10000,
    "nand_commands_per_query_median": 39.3021
  },
  {
    "dataset": "yfcc10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "yfcc10m-q3_t8-L50-r0-cold-flashanns-T4-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r1-cold-flashanns-T4-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r2-cold-flashanns-T4-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r3-cold-flashanns-T4-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r4-cold-flashanns-T4-4ad7-c3ef"
    ],
    "qps_median": 1423.86,
    "qps_ci_low": 1407.08,
    "qps_ci_high": 1439.32,
    "threads": 4,
    "committed_candidates_median": 50,
    "missing_pages_median": 44.5159,
    "issued_pages_per_query_median": 44.5159,
    "nand_read_bytes_median": 1650135040,
    "nq_median": 10000,
    "nand_commands_per_query_median": 39.3519
  },
  {
    "dataset": "yfcc10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "yfcc10m-q3_t8-L50-r0-cold-flashanns-T8-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r1-cold-flashanns-T8-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r2-cold-flashanns-T8-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r3-cold-flashanns-T8-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r4-cold-flashanns-T8-4ad7-c3ef"
    ],
    "qps_median": 3016.23,
    "qps_ci_low": 2992.67,
    "qps_ci_high": 3021.11,
    "threads": 8,
    "committed_candidates_median": 50,
    "missing_pages_median": 44.6206,
    "issued_pages_per_query_median": 44.6206,
    "nand_read_bytes_median": 1654214656,
    "nq_median": 10000,
    "nand_commands_per_query_median": 39.4514
  },
  {
    "dataset": "yfcc10m",
    "phase": "q3_t8",
    "system": "flashanns",
    "run_ids": [
      "yfcc10m-q3_t8-L50-r0-cold-flashanns-T16-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r1-cold-flashanns-T16-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r2-cold-flashanns-T16-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r3-cold-flashanns-T16-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r4-cold-flashanns-T16-4ad7-c3ef"
    ],
    "qps_median": 5985.38,
    "qps_ci_low": 5979.7,
    "qps_ci_high": 6008.27,
    "threads": 16,
    "committed_candidates_median": 50,
    "missing_pages_median": 44.8686,
    "issued_pages_per_query_median": 44.8686,
    "nand_read_bytes_median": 1661009920,
    "nq_median": 10000,
    "nand_commands_per_query_median": 39.6174
  },
  {
    "dataset": "yfcc10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "yfcc10m-q3_t8-L50-r0-cold-wise-only-T1-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r1-cold-wise-only-T1-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r2-cold-wise-only-T1-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r3-cold-wise-only-T1-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r4-cold-wise-only-T1-4ad7-c3ef"
    ],
    "qps_median": 320.67,
    "qps_ci_low": 312.56,
    "qps_ci_high": 324.25,
    "threads": 1,
    "committed_candidates_median": 50,
    "missing_pages_median": 44.413,
    "issued_pages_per_query_median": 44.413,
    "nand_read_bytes_median": 1647058944,
    "nq_median": 10000,
    "nand_commands_per_query_median": 39.278
  },
  {
    "dataset": "yfcc10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "yfcc10m-q3_t8-L50-r0-cold-wise-only-T2-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r1-cold-wise-only-T2-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r2-cold-wise-only-T2-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r3-cold-wise-only-T2-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r4-cold-wise-only-T2-4ad7-c3ef"
    ],
    "qps_median": 665.09,
    "qps_ci_low": 628.43,
    "qps_ci_high": 668.01,
    "threads": 2,
    "committed_candidates_median": 50,
    "missing_pages_median": 44.4438,
    "issued_pages_per_query_median": 44.4438,
    "nand_read_bytes_median": 1648087040,
    "nq_median": 10000,
    "nand_commands_per_query_median": 39.3023
  },
  {
    "dataset": "yfcc10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "yfcc10m-q3_t8-L50-r0-cold-wise-only-T4-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r1-cold-wise-only-T4-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r2-cold-wise-only-T4-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r3-cold-wise-only-T4-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r4-cold-wise-only-T4-4ad7-c3ef"
    ],
    "qps_median": 1329.59,
    "qps_ci_low": 1320.71,
    "qps_ci_high": 1334.06,
    "threads": 4,
    "committed_candidates_median": 50,
    "missing_pages_median": 44.5176,
    "issued_pages_per_query_median": 44.5176,
    "nand_read_bytes_median": 1650143232,
    "nq_median": 10000,
    "nand_commands_per_query_median": 39.3531
  },
  {
    "dataset": "yfcc10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "yfcc10m-q3_t8-L50-r0-cold-wise-only-T8-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r1-cold-wise-only-T8-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r2-cold-wise-only-T8-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r3-cold-wise-only-T8-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r4-cold-wise-only-T8-4ad7-c3ef"
    ],
    "qps_median": 2933.21,
    "qps_ci_low": 2922.31,
    "qps_ci_high": 2953.99,
    "threads": 8,
    "committed_candidates_median": 50,
    "missing_pages_median": 44.6192,
    "issued_pages_per_query_median": 44.6192,
    "nand_read_bytes_median": 1654214656,
    "nq_median": 10000,
    "nand_commands_per_query_median": 39.4517
  },
  {
    "dataset": "yfcc10m",
    "phase": "q3_t8",
    "system": "wise-only",
    "run_ids": [
      "yfcc10m-q3_t8-L50-r0-cold-wise-only-T16-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r1-cold-wise-only-T16-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r2-cold-wise-only-T16-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r3-cold-wise-only-T16-4ad7-c3ef",
      "yfcc10m-q3_t8-L50-r4-cold-wise-only-T16-4ad7-c3ef"
    ],
    "qps_median": 5791.61,
    "qps_ci_low": 5698.46,
    "qps_ci_high": 5951.88,
    "threads": 16,
    "committed_candidates_median": 50,
    "missing_pages_median": 44.8645,
    "issued_pages_per_query_median": 44.8645,
    "nand_read_bytes_median": 1660002304,
    "nq_median": 10000,
    "nand_commands_per_query_median": 39.5935
  }
]

DATASET_LABELS = {
    "t2i10m": "T2I-10M",
    "yfcc10m": "YFCC-10M",
    "laion10m": "LAION-10M",
}
COLORS = {
    "serial-t1": "#7f7f7f",
    "batch-t1": "#56b4e9",
    "extent-t1": "#0072b2",
    "wise-only": "#d55e00",
    "flashanns": "#0072b2",
}
LABELS = {
    "serial-t1": "Serialized",
    "batch-t1": "Post-commit batch",
    "extent-t1": "Co-use + extent",
    "wise-only": "Wise only",
    "flashanns": "FlashANNS",
}
PLOT_STYLE = {
    "font.size": 12,
    "axes.titlesize": 12,
    "axes.labelsize": 12,
    "xtick.labelsize": 10,
    "ytick.labelsize": 10,
    "legend.fontsize": 9,
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
    "savefig.bbox": "tight",
    "savefig.facecolor": "white",
}


def _row_key(row: dict) -> tuple:
    return (
        row["phase"],
        row["dataset"],
        row["system"],
        row.get("threads"),
    )


def validate_rows(rows: list[dict]) -> None:
    """Fail closed unless the complete Figure 9 evidence matrix is embedded."""
    if len(rows) != 33:
        raise ValueError(f"expected 33 Figure 9 marks, found {len(rows)}")

    q3_t1 = [row for row in rows if row["phase"] == "q3_t1"]
    q3_t8 = [row for row in rows if row["phase"] == "q3_t8"]
    if len(q3_t1) != 3 or len(q3_t8) != 30:
        raise ValueError("expected 3 q3_t1 marks and 30 q3_t8 marks")

    expected_t1 = {
        ("t2i10m", system)
        for system in ("serial-t1", "batch-t1", "extent-t1")
    }
    actual_t1 = {(row["dataset"], row["system"]) for row in q3_t1}
    if actual_t1 != expected_t1:
        raise ValueError(f"incomplete q3_t1 matrix: {actual_t1}")

    expected_t8 = {
        (dataset, system, threads)
        for dataset in DATASET_LABELS
        for system in ("wise-only", "flashanns")
        for threads in (1, 2, 4, 8, 16)
    }
    actual_t8 = {
        (row["dataset"], row["system"], int(row["threads"]))
        for row in q3_t8
    }
    if actual_t8 != expected_t8:
        raise ValueError("incomplete q3_t8 dataset/system/thread matrix")

    keys = [_row_key(row) for row in rows]
    if len(keys) != len(set(keys)):
        raise ValueError("duplicate aggregate mark key")

    all_run_ids = []
    for row in rows:
        if len(row["run_ids"]) != 5:
            raise ValueError(f"{_row_key(row)} does not name five runs")
        all_run_ids.extend(row["run_ids"])
        for key, value in row.items():
            if key.endswith(("_median", "_ci_low", "_ci_high")):
                if not isinstance(value, (int, float)) or not math.isfinite(value):
                    raise ValueError(f"non-finite {key} in {_row_key(row)}")
    if len(all_run_ids) != 165 or len(set(all_run_ids)) != 165:
        raise ValueError("expected 165 unique accepted run IDs")


def _style(ax, xlabel: str, ylabel: str, title: str) -> None:
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.set_title(title, loc="left", fontweight="bold")
    ax.grid(True, axis="y", color="#d9d9d9", linewidth=0.7)
    ax.set_axisbelow(True)


def _error(row: dict, key: str) -> tuple[float, float]:
    value = row[key]
    stem = key.removesuffix("_median")
    return value - row[f"{stem}_ci_low"], row[f"{stem}_ci_high"] - value


def _nand_mib_per_query(row: dict) -> float:
    return row["nand_read_bytes_median"] / row["nq_median"] / 1_048_576.0


def render_wise(rows: list[dict], output_dir: Path) -> tuple[Path, Path]:
    selected = [row for row in rows if row["phase"] == "q3_t1"]
    by_system = {row["system"]: row for row in selected}
    systems = ["serial-t1", "batch-t1", "extent-t1"]

    fig, axes = plt.subplots(1, 3, figsize=(7.4, 2.85), constrained_layout=True)

    values = [by_system[system]["qps_median"] for system in systems]
    errors = [_error(by_system[system], "qps_median") for system in systems]
    axes[0].bar(
        range(3),
        values,
        yerr=([err[0] for err in errors], [err[1] for err in errors]),
        capsize=2,
        color=[COLORS[system] for system in systems],
    )
    axes[0].set_xticks(
        range(3),
        [LABELS[system] for system in systems],
        rotation=18,
        ha="right",
    )
    _style(axes[0], "", "Throughput (QPS)", "(a) When")

    source = by_system["batch-t1"]
    funnel_keys = (
        "committed_candidates_median",
        "missing_pages_median",
        "issued_pages_per_query_median",
    )
    axes[1].bar(
        range(3),
        [source[key] for key in funnel_keys],
        color=("#999999", "#56b4e9", "#0072b2"),
    )
    axes[1].set_xticks(
        range(3),
        ("Committed", "Missing pages", "Issued pages"),
        rotation=18,
        ha="right",
    )
    axes[1].text(
        0.98,
        0.98,
        f"{_nand_mib_per_query(source):.2g} MiB/q\n"
        f"{source['nand_commands_per_query_median']:.2g} cmds/q",
        transform=axes[1].transAxes,
        va="top",
        ha="right",
        fontsize=7,
    )
    _style(axes[1], "", "Items/query", "(b) What")

    compare = ["batch-t1", "extent-t1"]
    x = np.arange(len(compare))
    width = 0.26
    metrics = (
        (lambda row: row["nand_commands_per_query_median"], "NAND commands"),
        (_nand_mib_per_query, "NAND MiB"),
        (lambda row: row["qps_median"], "QPS"),
    )
    for index, (value_of, label) in enumerate(metrics):
        baseline = value_of(by_system["batch-t1"])
        axes[2].bar(
            x + (index - 1) * width,
            [100 * value_of(by_system[system]) / baseline for system in compare],
            width,
            label=label,
        )
    axes[2].set_xticks(
        x,
        [LABELS[system] for system in compare],
        rotation=18,
        ha="right",
    )
    axes[2].set_ylim(bottom=0)
    axes[2].legend(fontsize=6, frameon=False)
    _style(axes[2], "", "Metric / post-commit batch (%)", "(c) How")

    pdf = output_dir / "wise-ablation.pdf"
    png = output_dir / "wise-ablation.png"
    fig.savefig(pdf)
    fig.savefig(png, dpi=200)
    plt.close(fig)
    return pdf, png


def render_batching(rows: list[dict], output_dir: Path) -> tuple[Path, Path]:
    scaling = [row for row in rows if row["phase"] == "q3_t8"]
    fig, axes = plt.subplots(1, 3, figsize=(7.4, 2.9), sharex=True)
    styles = {"wise-only": ("--", "o"), "flashanns": ("-", "s")}

    for ax, dataset in zip(axes, ("t2i10m", "yfcc10m", "laion10m")):
        selected = [row for row in scaling if row["dataset"] == dataset]
        for system in ("wise-only", "flashanns"):
            group = sorted(
                (row for row in selected if row["system"] == system),
                key=lambda row: row["threads"],
            )
            x = [row["threads"] for row in group]
            y = [row["qps_median"] for row in group]
            errors = [_error(row, "qps_median") for row in group]
            ax.errorbar(
                x,
                y,
                yerr=([err[0] for err in errors], [err[1] for err in errors]),
                color=COLORS[system],
                linestyle=styles[system][0],
                marker=styles[system][1],
                capsize=2,
                label=LABELS[system],
            )
            primary = next(row for row in group if row["threads"] == 8)
            ax.scatter(
                [8],
                [primary["qps_median"]],
                s=52,
                facecolor=COLORS[system],
                edgecolor="black",
                linewidth=0.7,
                zorder=4,
            )
        ax.axvline(8, color="#777777", linestyle=":", linewidth=0.9)
        ax.set_xticks((1, 2, 4, 8, 16))
        _style(
            ax,
            "Concurrent queries (T)",
            "Throughput (QPS)",
            DATASET_LABELS[dataset],
        )

    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(
        handles,
        labels,
        loc="upper center",
        ncol=2,
        frameon=False,
        bbox_to_anchor=(0.5, 0.99),
    )
    fig.subplots_adjust(
        left=0.09,
        right=0.99,
        bottom=0.20,
        top=0.72,
        wspace=0.38,
    )

    pdf = output_dir / "batching-ablation.pdf"
    png = output_dir / "batching-ablation.png"
    fig.savefig(pdf)
    fig.savefig(png, dpi=200)
    plt.close(fig)
    return pdf, png


def primary_t8_summary(rows: Iterable[dict]) -> list[tuple[str, str, float]]:
    return sorted(
        (
            DATASET_LABELS[row["dataset"]],
            LABELS[row["system"]],
            row["qps_median"],
        )
        for row in rows
        if row["phase"] == "q3_t8" and row["threads"] == 8
    )


def main() -> None:
    plt.rcParams.update(PLOT_STYLE)
    validate_rows(FIGURE9_ROWS)
    output_dir = Path(__file__).resolve().parent / "figure9-output"
    output_dir.mkdir(parents=True, exist_ok=True)
    outputs = (
        *render_wise(FIGURE9_ROWS, output_dir),
        *render_batching(FIGURE9_ROWS, output_dir),
    )
    for dataset, system, qps in primary_t8_summary(FIGURE9_ROWS):
        print(f"{dataset:10s} {system:10s} T=8 {qps:.2f} QPS")
    print(f"embedded marks: {len(FIGURE9_ROWS)}")
    print(f"accepted run IDs: {sum(len(row['run_ids']) for row in FIGURE9_ROWS)}")
    print(f"source aggregate SHA-256: {SOURCE_AGGREGATE_SHA256}")
    for path in outputs:
        print(f"wrote {path}")


if __name__ == "__main__":
    main()

