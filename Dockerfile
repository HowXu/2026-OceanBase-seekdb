# MiniVectorDB: build from source, run as a foreground stdin/stdout process.
# Only ubuntu:24.04 (the base image from the task statement) is pulled. The Go
# toolchain comes from the Ubuntu archive (golang-1.22-go 1.22.2-2), and the
# code uses only the Go standard library, so the build needs no Go proxy.

FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
# Fall back to a domestic mirror if the default archive is unreachable.
RUN (apt-get update || (sed -i 's#http://\(archive\|security\).ubuntu.com#http://mirrors.aliyun.com#g' \
        /etc/apt/sources.list.d/ubuntu.sources && apt-get update)) && \
    apt-get install -y --no-install-recommends golang-1.22-go && \
    rm -rf /var/lib/apt/lists/*
ENV PATH=/usr/lib/go-1.22/bin:$PATH CGO_ENABLED=0 GOOS=linux GOARCH=amd64 \
    GOPROXY=off GOTOOLCHAIN=local GOFLAGS=-trimpath
WORKDIR /src
COPY minivectordb/ ./
RUN go build -ldflags="-s -w" -o /out/minivectordb ./cmd/minivectordb

FROM ubuntu:24.04
WORKDIR /app
COPY --from=build /out/minivectordb /app/minivectordb
RUN chmod 755 /app/minivectordb && mkdir -p /data
VOLUME ["/data"]
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]
