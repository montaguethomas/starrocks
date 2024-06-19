# This docker file build the Starrocks artifacts fe & be and package them into a busybox-based image
# Please run this command from the git repo root directory to build:
#
# Build a CentOS7 based artifact image:
#  > DOCKER_BUILDKIT=1 docker build --rm=true --build-arg builder=starrocks/dev-env-centos7:latest -f docker/dockerfiles/artifacts/artifact.Dockerfile -t artifacts-centos7:latest .
#
# Build a Ubuntu based artifact image:
#  > DOCKER_BUILDKIT=1 docker build --rm=true --build-arg builder=starrocks/dev-env-ubuntu:latest -f docker/dockerfiles/artifacts/artifact.Dockerfile -t artifacts-ubuntu:latest .

ARG builder=starrocks/dev-env-ubuntu:latest
ARG RELEASE_VERSION
ARG BUILD_TYPE=Release
ARG CUSTOM_MVN
ARG MAVEN_OPTS="-Dmaven.artifact.threads=128"
ARG BUILD_ROOT=/build


FROM ${builder} as builder
ARG BUILD_ROOT
COPY . ${BUILD_ROOT}
WORKDIR ${BUILD_ROOT}


FROM builder as fe-builder
ARG RELEASE_VERSION
ARG BUILD_TYPE
ARG CUSTOM_MVN
ARG MAVEN_OPTS
# clean and build Frontend and Spark Dpp application
RUN --mount=type=cache,target=/root/.m2/ STARROCKS_VERSION=${RELEASE_VERSION} BUILD_TYPE=${BUILD_TYPE} CUSTOM_MVN=${CUSTOM_MVN} MAVEN_OPTS=${MAVEN_OPTS} ./build.sh --fe --clean


FROM builder as broker-builder
ARG RELEASE_VERSION
ARG CUSTOM_MVN
ARG MAVEN_OPTS
# clean and build Frontend and Spark Dpp application
RUN --mount=type=cache,target=/root/.m2/ cd fs_brokers/apache_hdfs_broker/ && STARROCKS_VERSION=${RELEASE_VERSION} CUSTOM_MVN=${CUSTOM_MVN} MAVEN_OPTS=${MAVEN_OPTS} ./build.sh


FROM builder as be-builder
ARG RELEASE_VERSION
ARG BUILD_TYPE
ARG CUSTOM_MVN
ARG MAVEN_OPTS
RUN --mount=type=cache,target=/root/.m2/ STARROCKS_VERSION=${RELEASE_VERSION} BUILD_TYPE=${BUILD_TYPE} CUSTOM_MVN=${CUSTOM_MVN} MAVEN_OPTS=${MAVEN_OPTS} ./build.sh --be --enable-shared-data --clean -j `nproc`


FROM ubuntu:22.04 as datadog-downloader
RUN apt-get update -y && apt-get install -y --no-install-recommends curl tar xz-utils

# download the latest dd-java-agent
ADD 'https://dtdg.co/latest-java-tracer' /datadog/dd-java-agent.jar

# Get ddprof for BE profiling
RUN imagearch=$(arch | sed 's/aarch64/arm64/; s/x86_64/amd64/') \
    && curl -so ddprof-linux.tar.xz "https://github.com/DataDog/ddprof/releases/latest/download/ddprof-${imagearch}-linux.tar.xz" \
    && tar xvf ddprof-linux.tar.xz && mkdir -p /datadog/  \
    && mv ddprof/bin/ddprof /datadog/ \
    && chmod 755 /datadog/ddprof


FROM busybox:latest
ARG RELEASE_VERSION
ARG BUILD_ROOT

LABEL org.opencontainers.image.source="https://github.com/starrocks/starrocks"
LABEL org.starrocks.version=${RELEASE_VERSION:-"UNKNOWN"}

COPY --from=fe-builder ${BUILD_ROOT}/output /release/fe_artifacts
COPY --from=be-builder ${BUILD_ROOT}/output /release/be_artifacts
COPY --from=broker-builder ${BUILD_ROOT}/fs_brokers/apache_hdfs_broker/output /release/broker_artifacts

COPY --from=datadog-downloader /datadog/dd-java-agent.jar /release/fe_artifacts/fe/datadog/dd-java-agent.jar
COPY --from=datadog-downloader /datadog/ddprof /release/be_artifacts/be/datadog/ddprof

WORKDIR /release
