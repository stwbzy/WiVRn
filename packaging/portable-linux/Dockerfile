FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install --yes --no-install-recommends \
      bsdextrautils build-essential ca-certificates cmake curl desktop-file-utils file git \
      glslang-tools libarchive-dev libavahi-client-dev libavahi-glib-dev \
      libavcodec-dev libavutil-dev libboost-all-dev libbsd-dev libcli11-dev \
      libdrm-dev libeigen3-dev libfuse2 libglib2.0-dev-bin libnotify-dev \
      libpipewire-0.3-dev librsvg2-dev libssl-dev libsystemd-dev libudev-dev \
      libvulkan-dev libwayland-dev libx264-dev libxrandr-dev ninja-build \
      nlohmann-json3-dev patchelf pkg-config python3 squashfs-tools spirv-tools \
 && rm -rf /var/lib/apt/lists/*

# Ubuntu 24.04 provides a suitably old glibc for a portable artifact, but its
# Vulkan headers predate the version required by current WiVRn/Monado.
ARG VULKAN_HEADERS_TAG=vulkan-sdk-1.4.328.1
RUN curl --fail --location --retry 3 \
      "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${VULKAN_HEADERS_TAG}.tar.gz" \
      --output /tmp/vulkan-headers.tar.gz \
 && mkdir /tmp/vulkan-headers \
 && tar --extract --gzip --file /tmp/vulkan-headers.tar.gz \
      --strip-components=1 --directory /tmp/vulkan-headers \
 && cmake -S /tmp/vulkan-headers -B /tmp/vulkan-headers-build \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
 && cmake --install /tmp/vulkan-headers-build \
 && rm -rf /tmp/vulkan-headers /tmp/vulkan-headers-build /tmp/vulkan-headers.tar.gz

WORKDIR /src
