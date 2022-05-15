python ./scripts/run.py --mode nerf \
    --scene ./data/nerf/fox \
    --n_steps 3000 \
    --screenshot_transforms ./data/nerf/fox/transforms.json \
    --nerfporter \
    --nerfporter_color_dir ./output/fox/color \
    --nerfporter_depth_dir ./output/fox/depth \
    --screenshot_spp 1