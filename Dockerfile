FROM alpine:3.21 AS build

RUN apk add --no-cache g++ make
WORKDIR /build
COPY Makefile ./
COPY src ./src
RUN make miniredis

FROM alpine:3.21

WORKDIR /data
COPY --from=build /build/miniredis /usr/local/bin/miniredis

EXPOSE 6380
VOLUME ["/data"]

ENTRYPOINT ["/usr/local/bin/miniredis"]
CMD ["6380"]
