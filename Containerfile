# Stage to copy build files
FROM scratch AS ctx
COPY build_files /

# Base Image
FROM ghcr.io/ublue-os/bazzite-nvidia:latest

# Build arguments for module signing secrets
ARG module_signing_key
ARG module_signing_crt  
ARG module_signing_der

# Create temporary directory for secrets and decode them
RUN --mount=type=bind,from=ctx,source=/,target=/ctx \
    --mount=type=cache,dst=/var/cache \
    --mount=type=cache,dst=/var/log \
    --mount=type=tmpfs,dst=/tmp \
    mkdir -p /tmp/secrets && \
    echo "$module_signing_key" | base64 -d > /tmp/secrets/module-signing.key && \
    echo "$module_signing_crt" | base64 -d > /tmp/secrets/module-signing.crt && \
    echo "$module_signing_der" | base64 -d > /tmp/secrets/module-signing.der && \
    /ctx/build.sh && \
    rm -rf /tmp/secrets && \
    ostree container commit

RUN bootc container lint
