#!/bin/bash

docker build -t photinobuilder .
docker run -it --rm -v $(pwd):/src photinobuilder \
	make linux-x64 \
	&& dotnet pack Photino.Native/Photino.Native.csproj
