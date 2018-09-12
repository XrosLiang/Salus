# -*- coding: future_fstrings -*-
"""
Measure inference latency vs concurrency. See card#259

LaneMgr: enabled
InLane Scheduler: fair
Collected data: per iteration time (latency)
"""
from __future__ import absolute_import, print_function, division, unicode_literals

import random
from absl import flags

from benchmarks.driver.runner import Executor
from benchmarks.driver.server.config import presets
from benchmarks.driver.utils import execute
from benchmarks.driver.utils.prompt import pause
from benchmarks.driver.workload import WTL
from benchmarks.exps import run_seq, maybe_forced_preset, Pause, run_tf, run_tfdist

from benchmarks.driver.utils.compatiblity import pathlib


FLAGS = flags.FLAGS

rates = [100, 75, 50, 25, 1]


def case1():
    for rate in rates:
        wl = WTL.create("inception3eval", 1, 500, executor=Executor.TFDist)
        wl.env['SALUS_TFBENCH_EVAL_INTERVAL'] = str(1 / rate)
        wl.env['SALUS_TFBENCH_EVAL_RAND_FACTOR'] = '1'
        wl.env['SALUS_TFBENCH_EVAL_BLOCK'] = 'false'
        run_tfdist(FLAGS.save_dir/'case1'/str(rate), wl)


def case2():
    scfg = maybe_forced_preset(presets.MostEfficient)

    for rate in rates:
        wl = WTL.create("inception3eval", 1, 500, executor=Executor.Salus)
        wl.env['SALUS_TFBENCH_EVAL_INTERVAL'] = str(1 / rate)
        print("using interval " + str(1 / rate))
        wl.env['SALUS_TFBENCH_EVAL_RAND_FACTOR'] = '1'
        wl.env['SALUS_TFBENCH_EVAL_BLOCK'] = 'false'
        run_seq(scfg.copy(output_dir=FLAGS.save_dir/'case2'/str(rate)), wl)


def case3():
    pause("Please start MPS server")

    for rate in rates:
        wl = WTL.create("inception3eval", 1, 500, executor=Executor.TFDist)
        wl.env['SALUS_TFBENCH_EVAL_INTERVAL'] = str(1 / rate)
        wl.env['SALUS_TFBENCH_EVAL_RAND_FACTOR'] = '1'
        wl.env['SALUS_TFBENCH_EVAL_BLOCK'] = 'false'
        run_tfdist(FLAGS.save_dir/'case3'/str(rate), wl)


def main(argv):
    command = argv[0] if argv else "test"

    {
        "case1": case1,
        "case2": case2,
        "case3": case3,
    }[command]()
