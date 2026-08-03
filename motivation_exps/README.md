# CXL SSD × ANNS Motivation

唯一有效路径：`/dev/vmem0` 统一地址空间（mmap + 计数器）。

```bash
cd /root/chukexin/motivation_exps
./motivation vmem-identity
./scripts/run_vmem_laion.sh
```

结论：[`results/MOTIVATION_FINDINGS.md`](results/MOTIVATION_FINDINGS.md)
