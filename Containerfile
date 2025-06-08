# Stage to copy build files
FROM scratch AS ctx
COPY build_files /

# Base Image
FROM ghcr.io/ublue-os/bazzite-nvidia:latest

# Use BuildKit secrets to mount keys during build
RUN --mount=type=secret,id=module_signing_key,target=/tmp/module_signing.key.b64 \
    --mount=type=secret,id=module_signing_crt,target=/tmp/module_signing.crt.b64 \
    --mount=type=secret,id=module_signing_der,target=/tmp/module_signing.der.b64 \
    mkdir -p /etc/pki/module-signing && \
    base64 -d /tmp/module_signing.key.b64 > /etc/pki/module-signing/module-signing.key && \
    base64 -d /tmp/module_signing.crt.b64 > /etc/pki/module-signing/module-signing.crt && \
    base64 -d /tmp/module_signing.der.b64 > /etc/pki/module-signing/module-signing.der && \
    chmod 600 /etc/pki/module-signing/module-signing.key && \
    # run your build script that uses these files
    /ctx/build.sh && \
    ostree container commit


RUN bootc container lint
