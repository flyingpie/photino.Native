FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
	build-essential \
	libgtk-3-dev \
	libwebkit2gtk-4.1-dev \
	libnotify4 \
	libnotify-dev

RUN apt-get update && apt get install -y \
	dotnet-sdk-8.0

WORKDIR /src
