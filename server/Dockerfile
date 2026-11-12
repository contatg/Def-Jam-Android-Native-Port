FROM golang:1.27.3-alpine AS build
WORKDIR /src
COPY . .
RUN CGO_ENABLED=0 go build -buildvcs=false -trimpath -o /defjam-server ./cmd/defjam-server
FROM alpine:3.22
RUN adduser -D -u 10001 game
USER game
COPY --from=build /defjam-server /usr/local/bin/defjam-server
EXPOSE 27777/udp
ENTRYPOINT ["defjam-server"]
CMD ["-listen",":27777"]
