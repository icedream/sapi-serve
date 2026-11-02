# sapi_serve: SAPI 5 voices behind an HTTP endpoint. No voice data in the image:
# Microsoft owns those files, so they are mounted at runtime (see k8s.yaml).
#
#   docker build -f serve/Dockerfile -t sapi-serve:0 .     (from the repository root)
#   docker run -p 8080:8080 --read-only --cap-drop ALL \
#     -v /path/to/voice:/voice:ro sapi-serve:0
#
# HTTP framing comes from libmicrohttpd and JSON from cJSON, both packaged in Alpine.
# The base is pinned by digest: those libraries arrive through the distribution and inherit
# its CVEs, so the base has to be diffable.
FROM alpine:3.22@sha256:2e0b81d60597f449baa56dc25d355367869d2dca4cd57e866f1e2b52af3eb55f AS build
RUN apk add --no-cache gcc make musl-dev pkgconf libmicrohttpd-dev cjson-dev
COPY third_party/ms-sam-mike-mary-decomp /src/ms
COPY serve /src/serve
# .git is not in the context, so the version has to come in as a build arg
ARG VERSION=docker
ARG ENGINE_COMMIT=unknown
RUN make -C /src/ms && make -C /src/serve SAM=/src/ms VERSION="$VERSION" \
      ENGINE_COMMIT="$ENGINE_COMMIT" \
      LDFLAGS="-L/src/ms/build -lsam -lm -Wl,-rpath,/usr/lib"

FROM alpine:3.22@sha256:2e0b81d60597f449baa56dc25d355367869d2dca4cd57e866f1e2b52af3eb55f
RUN apk add --no-cache libgcc libmicrohttpd cjson
COPY --from=build /src/ms/build/libsam.so /usr/lib/
COPY --from=build /src/serve/sapi_serve /usr/local/bin/sapi_serve
ENV SAM_DATA_DIR=/voice \
    SAM_PORT=8080 \
    SAM_MAX_SECONDS=120
EXPOSE 8080
USER 65534:65534
ENTRYPOINT ["/usr/local/bin/sapi_serve"]
