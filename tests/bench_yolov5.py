#!/usr/bin/env python3
"""
bench_yolov5.py — Benchmark YOLOv5s inference on RKNN NPU

Usage: python3 bench_yolov5.py [model_path] [num_runs]

Default model: RK3566_RK3568/yolov5s-640-640.rknn from RKNN toolkit
Default runs:  20
"""
import sys
import time
import numpy as np
from rknnlite.api import RKNNLite

DEFAULT_MODEL = (
    '/root/work/npu2/rknn-toolkit2-v2.4.0/'
    'rknn-toolkit2-v2.4.0-2026-01-17/rknpu2/'
    'examples/rknn_yolov5_demo/model/RK3566_RK3568/yolov5s-640-640.rknn'
)

def main():
    model_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_MODEL
    num_runs = int(sys.argv[2]) if len(sys.argv) > 2 else 20

    rknn = RKNNLite()
    ret = rknn.load_rknn(model_path)
    if ret != 0:
        print(f'ERROR: load_rknn failed: {ret}')
        return 1

    ret = rknn.init_runtime()
    if ret != 0:
        print(f'ERROR: init_runtime failed: {ret}')
        return 1

    # NHWC input
    inp = np.random.randint(0, 255, (1, 640, 640, 3), dtype=np.uint8)

    # Warmup
    for _ in range(5):
        rknn.inference(inputs=[inp])

    # Benchmark
    times = []
    for _ in range(num_runs):
        t0 = time.monotonic()
        rknn.inference(inputs=[inp])
        t1 = time.monotonic()
        times.append((t1 - t0) * 1000)

    print(f'YOLOv5s 640x640 ({num_runs} runs):')
    print(f'  Min:  {min(times):.1f} ms')
    print(f'  Max:  {max(times):.1f} ms')
    print(f'  Avg:  {sum(times)/len(times):.1f} ms')
    print(f'  FPS:  {1000/(sum(times)/len(times)):.1f}')

    rknn.release()
    return 0

if __name__ == '__main__':
    sys.exit(main())
