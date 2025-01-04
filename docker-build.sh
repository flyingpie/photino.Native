#!/bin/bash

docker build -t photinobuilder .
docker run -it --rm -v $(pwd):/src photinobuilder bash
