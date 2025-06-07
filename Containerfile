# Allow build scripts to be referenced without being copied into the final image
FROM scratch AS ctx
COPY build_files /

# Inject GitHub secrets as build args
ARG BAZZITE_MODULE_SIGNING_KEY
ARG BAZZITE_MODULE_SIGNING_CRT
ARG BAZZITE_MODULE_SIGNING_DER

ENV BAZZITE_MODULE_SIGNING_KEY=${BAZZITE_MODULE_SIGNING_KEY}
ENV BAZZITE_MODULE_SIGNING_CRT=${BAZZITE_MODULE_SIGNING_CRT}
ENV BAZZITE_MODULE_SIGNING_DER=${BAZZITE_MODULE_SIGNING_DER}

# Base Image
FROM ghcr.io/ublue-os/bazzite-nvidia:latest

### MODIFICATIONS
RUN --mount=type=bind,from=ctx,source=/,target=/ctx \
    --mount=type=cache,dst=/var/cache \
    --mount=type=cache,dst=/var/log \
    --mount=type=tmpfs,dst=/tmp \
    /ctx/build.sh && \
    ostree container commit

### LINTING
RUN bootc container lint
