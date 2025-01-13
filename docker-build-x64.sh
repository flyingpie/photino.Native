#!/bin/bash

docker build -t photinobuilder:x64 .

docker run -it --rm -v $(pwd):/src photinobuilder:x64 make linux-x64
