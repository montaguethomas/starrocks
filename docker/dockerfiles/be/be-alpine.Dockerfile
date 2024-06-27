# This docker file build the Starrocks be alpine image
#
ARG ARTIFACT_SOURCE=image

ARG ARTIFACTIMAGE=starrocks/artifacts-alpine:latest
FROM ${ARTIFACTIMAGE} as artifacts-from-image

# create a docker build stage that copy locally build artifacts
FROM scratch as artifacts-from-local
ARG LOCAL_REPO_PATH
COPY ${LOCAL_REPO_PATH}/output/be /release/be_artifacts/be


FROM artifacts-from-${ARTIFACT_SOURCE} as artifacts
RUN rm -f /release/be_artifacts/be/lib/starrocks_be.debuginfo


FROM alpine/java:11-jdk
ARG STARROCKS_ROOT=/opt/starrocks
CMD ["/bin/bash"]

# Upgrade and install required packages
RUN apk upgrade --no-cache && apk add --no-cache bash curl libc6-compat mysql-client openjdk8-jre-base

# Run as starrocks user
ARG USER=starrocks
RUN adduser -h ${STARROCKS_ROOT} -u 1001 -D -s /usr/sbin/nologin ${USER}
USER ${USER}
WORKDIR ${STARROCKS_ROOT}

# Copy all artifacts to the runtime container image
COPY --from=artifacts --chown=${USER}:${USER} /release/be_artifacts/ $STARROCKS_ROOT/

# Copy be k8s scripts to the runtime container image
COPY --chown=${USER}:${USER} docker/dockerfiles/be/*.sh $STARROCKS_ROOT/

# Create directory for BE storage, create cn symbolic link to be
RUN mkdir -p ${STARROCKS_ROOT}/be/storage && ln -sfT be ${STARROCKS_ROOT}/cn

# run as root by default
USER root
