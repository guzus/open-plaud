# Build a static, dependency-free binary, then ship it on distroless.
FROM golang:1.22-alpine AS build
WORKDIR /src
COPY go.mod ./
COPY main.go ./
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /bin/open-plaud-ingest .

FROM gcr.io/distroless/static-debian12
COPY --from=build /bin/open-plaud-ingest /open-plaud-ingest
# Railway injects PORT; storage defaults to the mounted volume at /data.
ENV OP_STORAGE_DIR=/data
EXPOSE 8000
ENTRYPOINT ["/open-plaud-ingest"]
