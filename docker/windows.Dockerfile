# Builds the Windows binaries with the real MSVC toolchain, run under Wine (msvc-wine).
#
# vsdownload.py downloads MSVC and the Windows SDK from Microsoft. Building this image accepts the
# Microsoft Visual Studio license on your behalf: https://go.microsoft.com/fwlink/?LinkId=2086102
FROM debian:trixie-slim

ARG DEBIAN_MIRROR=deb.debian.org
ARG MSVC_WINE_COMMIT=514f8ea34842cd6d831804d0e9658d3a32870ae1
# Visual Studio 2022, as used by the VPC builds on GitHub Actions
ARG VS_MAJOR=17

RUN sed -i "s|deb.debian.org|${DEBIAN_MIRROR}|g" /etc/apt/sources.list.d/debian.sources \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      wine wine64 python3 msitools ca-certificates winbind cmake ninja-build git \
 && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/mstorsjo/msvc-wine /tmp/msvc-wine \
 && git -C /tmp/msvc-wine checkout -q ${MSVC_WINE_COMMIT} \
 && cd /tmp/msvc-wine \
 && PYTHONUNBUFFERED=1 ./vsdownload.py --accept-license --major ${VS_MAJOR} \
      --architecture x86 x64 --dest /opt/msvc \
 && ./install.sh /opt/msvc \
 && rm -rf /tmp/msvc-wine

# build.sh points WINEPREFIX into the build directory, so the image can run as any user
ENV WINEDEBUG=-all
