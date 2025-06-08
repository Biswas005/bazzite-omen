# Stage to copy build files
FROM scratch AS ctx
COPY build_files /

# Base Image
FROM ghcr.io/ublue-os/bazzite-nvidia:latest

# Use BuildKit secrets to mount keys during build
RUN --mount=type=bind,from=ctx,source=/,target=/ctx \
    --mount=type=cache,dst=/var/cache \
    --mount=type=cache,dst=/var/log \
    --mount=type=tmpfs,dst=/tmp \
    --mount=type=secret,id=module_signing_key,target=/run/secrets/module-signing.key.b64 \
    --mount=type=secret,id=module_signing_crt,target=/run/secrets/module-signing.crt.b64 \
    --mount=type=secret,id=module_signing_der,target=/run/secrets/module-signing.der.b64 \
    /ctx/build.sh && \
    ostree container commit

RUN bootc container lint
