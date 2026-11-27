# Scheduler image. Build stage and runtime share Debian 12 so the glibc and
# libstdc++ the binary was linked against are exactly what the runtime has.
FROM debian:12 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ cmake ninja-build ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY src ./src
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNRS_BUILD_TESTS=OFF \
 && cmake --build /build --target nr_mac_scheduler \
 && strip /build/nr_mac_scheduler

# distroless/cc: glibc + libstdc++ only. No shell or package manager, so there is
# much less attack surface, and the :nonroot tag runs as uid 65532.
FROM gcr.io/distroless/cc-debian12:nonroot
COPY --from=build /build/nr_mac_scheduler /usr/local/bin/nr_mac_scheduler
COPY config/default.yaml /etc/nrs/config.yaml
USER 65532:65532
EXPOSE 38472/udp 8080/tcp
ENTRYPOINT ["/usr/local/bin/nr_mac_scheduler", "--config", "/etc/nrs/config.yaml"]
