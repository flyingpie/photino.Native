#!/bin/bash

docker build -t photinobuilder:arm64 --platform linux/arm64 .

docker run -it --rm -v $(pwd):/src --platform linux/arm64 photinobuilder:arm64 make linux-arm64
