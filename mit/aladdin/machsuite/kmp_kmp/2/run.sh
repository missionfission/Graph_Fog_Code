#!/bin/sh \
/workspace/gem5-aladdin/build/X86/gem5.opt \
--stats-db-file=stats.db \
--outdir=/workspace/gem5-aladdin/sweeps/machsuite/kmp_kmp/2/outputs \
/workspace/gem5-aladdin/configs/aladdin/aladdin_se.py \
--num-cpus=0 \
--mem-size=4GB \
--enable-stats-dump \
--enable_prefetchers \
--prefetcher-type=stride \
--mem-type=DDR3_1600_8x8  \
--sys-clock=249MHz \
--cpu-type=DerivO3CPU  \
--caches \
 \
--cacheline_size=32  \
 \
 \
--accel_cfg_file=/workspace/gem5-aladdin/sweeps/machsuite/kmp_kmp/2/gem5.cfg \
-c kmp-kmp-gem5-accel -o "" \
> /workspace/gem5-aladdin/sweeps/machsuite/kmp_kmp/2/outputs/stdout \
2> /workspace/gem5-aladdin/sweeps/machsuite/kmp_kmp/2/outputs/stderr