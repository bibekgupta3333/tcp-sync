# Linux dev image with the tools the macOS host lacks: g++, gdb, valgrind.
#   make docker-check                                  # test + asan + tsan + valgrind
#   docker run --rm -it --cap-add=SYS_PTRACE tcpsync-dev bash   # interactive gdb session
FROM ubuntu:24.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        g++ make cmake gdb valgrind netcat-openbsd util-linux ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN make clean && make -j"$(nproc)" all

CMD ["make", "test"]
