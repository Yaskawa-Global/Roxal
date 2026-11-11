"""A few frames of a LIBERO episode, for examples/triton/smolvla_libero.rox.

For each frame asked for, a directory frame_NNNN holding image.png and
image2.png (the two cameras, as recorded) and sample.json: the task, the
robot's state, and the actions recorded from that frame on (up to --chunk of
them, the length of the SmolVLA's action chunk).
"""
import argparse
import json
import os

import numpy as np
from PIL import Image
from lerobot.datasets.lerobot_dataset import LeRobotDataset


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default="lerobot/libero")
    parser.add_argument("--episode", type=int, default=0)
    parser.add_argument("--frames", type=int, nargs="+", default=[0, 40, 80])
    parser.add_argument("--chunk", type=int, default=50)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    dataset = LeRobotDataset(args.repo, episodes=[args.episode])
    frames = [dataset[i] for i in range(len(dataset))]
    frames = [f for f in frames if int(f["episode_index"]) == args.episode]
    print(f"episode {args.episode}: {len(frames)} frames, task: {frames[0]['task']}")

    for k in args.frames:
        if k >= len(frames):
            print(f"frame {k}: past the episode's end, skipped")
            continue
        frame = frames[k]
        out = os.path.join(args.out, f"frame_{k:04d}")
        os.makedirs(out, exist_ok=True)
        for key, name in (("observation.images.image", "image.png"), ("observation.images.image2", "image2.png")):
            # float [C, H, W] in 0..1 -> uint8 [H, W, C]
            pixels = (frame[key].permute(1, 2, 0).numpy() * 255.0).round().clip(0, 255).astype(np.uint8)
            Image.fromarray(pixels).save(os.path.join(out, name))
        actions = [frames[j]["action"].tolist() for j in range(k, min(k + args.chunk, len(frames)))]
        with open(os.path.join(out, "sample.json"), "w") as f:
            json.dump({"task": frame["task"], "state": frame["observation.state"].tolist(),
                       "actions": actions}, f)
        print(f"frame {k}: {out}")


if __name__ == "__main__":
    main()
