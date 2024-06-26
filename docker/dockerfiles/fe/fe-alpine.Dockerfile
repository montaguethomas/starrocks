
ARG ARTIFACT_SOURCE=image

ARG ARTIFACTIMAGE=starrocks/artifacts-alpine:latest
FROM ${ARTIFACTIMAGE} as artifacts-from-image

# create a docker build stage that copy locally build artifacts
FROM scratch as artifacts-from-local
ARG LOCAL_REPO_PATH
COPY ${LOCAL_REPO_PATH}/output/fe /release/fe_artifacts/fe


FROM artifacts-from-${ARTIFACT_SOURCE} as artifacts


FROM alpine/java:11-jdk
ARG STARROCKS_ROOT=/opt/starrocks
CMD ["/bin/bash"]

# Upgrade and install required packages
RUN apk upgrade --no-cache && apk add --no-cache bash curl libc6-compat mysql-client openjdk8-jre-base

# Run as starrocks user
ARG USER=starrocks
RUN adduser -h ${STARROCKS_ROOT} -u 1000 -D -s /usr/sbin/nologin ${USER}
USER ${USER}
WORKDIR ${STARROCKS_ROOT}

# Copy all artifacts to the runtime container image
COPY --from=artifacts --chown=${USER}:${USER} /release/fe_artifacts/ $STARROCKS_ROOT/

# Copy fe k8s scripts to the runtime container image
COPY --chown=${USER}:${USER} docker/dockerfiles/fe/*.sh $STARROCKS_ROOT/

# Create directory for FE metadata
RUN mkdir -p /opt/starrocks/fe/meta

# run as root by default
USER root
