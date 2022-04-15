import numpy as np
import argparse
import os, shutil
from PIL import Image
from glob import glob


def add_perturbation(img, seed):
    np.random.seed(seed)
    img_np = np.array(img)/255.0
    s = np.random.uniform(0.8, 1.2, size=3)
    b = np.random.uniform(-0.2, 0.2, size=3)
    img_np[..., :3] = np.clip(s*img_np[..., :3]+b, 0, 1)
    img = Image.fromarray((255*img_np).astype(np.uint8))
    return img


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', required=True, type=str)
    parser.add_argument('--output', required=True, type=str)
    args = parser.parse_args()

    dirs = os.listdir(args.input)
    if 'train' in dirs:
        img_paths = sorted(glob(os.path.join(args.input, 'train/*.png')))
    elif 'images' in dirs:
        img_paths = sorted(glob(os.path.join(args.input, 'images/*.png')))
    
    shutil.copytree(args.input, args.output)

    for idx, img_path in enumerate(img_paths):
        img = Image.open(img_path)
        img_perturbed = add_perturbation(img, idx)
        output_path = img_path.replace(args.input, args.output)
        img_perturbed.save(output_path, quality=100, subsampling=0)


