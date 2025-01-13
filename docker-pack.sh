#!/bin/bash

docker build -t photinobuilder:x64 --platform amd64 .

docker run -it --rm -v $(pwd):/src --platform amd64 photinobuilder:x64 dotnet pack Photino.Native/Photino.Native.csproj --output .
