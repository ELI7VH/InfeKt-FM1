/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 J0KERPACK */
/* the chip's microbenchmark (bench.c): cycles an op x 100, in this order (a MAC: a multiply and an add) */
#pragma once
#include <stdint.h>
enum { BENCH_MAC_FL, BENCH_MAC_RAM, BENCH_ADD, BENCH_MUL, BENCH_DIV, BENCH_CVT, BENCH_LD_RAM, BENCH_LD_FL4K, BENCH_LD_FL64K,
       BENCH_INT, BENCH_LD_SEQ, BENCH_ST_SEQ, BENCH_FSCALE, BENCH_LD_POOL, BENCH_LDSEQ_POOL,
       BENCH_MAC_DEN, BENCH_ADD_DEN, BENCH_LD_COLD, BENCH_CODE_COLD, BENCH_CODE_WARM,
       BENCH_MAC_IND, BENCH_BRANCH, BENCH_N };
/* pool: 1024 words of the pool, written */
int hyb_bench(uint32_t (*clock)(void), uint32_t cycles_a_tick, uint32_t *pool, uint32_t res[BENCH_N]);
