# arc_os build environment: the same tools as CI and cloud sessions.
#
#   docker build -t arc_os-dev .
#   docker run --rm -it -v "$PWD":/src -w /src arc_os-dev
#   # inside: tools/dev-setup.sh limine   (once per checkout; Limine lives in ./limine)
#   #         cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake && cmake --build build
FROM ubuntu:24.04

# Only the setup script and its lock go into the image, so source edits don't bust the cache.
COPY tools/dev-setup.sh tools/toolchain.lock /opt/arc-setup/tools/
RUN apt-get update -qq \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends ca-certificates curl \
 && bash /opt/arc-setup/tools/dev-setup.sh apt \
 && bash /opt/arc-setup/tools/dev-setup.sh toolchain \
 && rm -rf /var/lib/apt/lists/*

# Git refuses to work in a bind-mounted checkout owned by another user without this.
RUN git config --system --add safe.directory '*'

CMD ["bash"]
