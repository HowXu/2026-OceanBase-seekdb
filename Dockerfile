# MiniVectorDB: build from source, run as a foreground stdin/stdout process.
# Base images are pinned by digest (multi-arch index) for reproducible builds.

FROM golang:1.26-bookworm@sha256:5de7c99eb78ca5e515a4c340235d0955ec5964a5071c492c15503d7c267da473 AS build
WORKDIR /src
COPY minivectordb/ ./
# Standard library only, so the build needs no network access.
ENV CGO_ENABLED=0 GOOS=linux GOARCH=amd64 GOPROXY=off GOTOOLCHAIN=local
RUN go vet ./... && go test ./... && \
    go build -trimpath -ldflags="-s -w" -o /out/minivectordb ./cmd/minivectordb

FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
WORKDIR /app
COPY --from=build --chmod=755 /out/minivectordb /app/minivectordb
RUN mkdir -p /data
VOLUME ["/data"]
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]
