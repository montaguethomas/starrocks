# This docker file build the Starrocks artifacts fe & be and package them into a scratch image
# Please run this command from the git repo root directory to build:
#

ARG BUILDER_IMAGE=alpine/java:11-jdk
ARG RELEASE_VERSION
ARG BUILD_TYPE=Release
ARG CUSTOM_MVN
ARG MAVEN_OPTS="-Dmaven.artifact.threads=128"
ARG BUILD_ROOT=/build


FROM ${BUILDER_IMAGE} as builder
ARG BUILD_ROOT
ENV STARROCKS_THIRDPARTY=/var/local/thirdparty
# Common Packages used in final images
RUN apk add --no-cache bash curl libc6-compat mysql-client openjdk8-jre-base
# Build packages
# byacc conflicts with bison due to both providing /usr/bin/yacc. byacc is just a symlink to yacc and bison's yacc is suppose to be compatible.
RUN apk add --no-cache abseil-cpp autoconf automake binutils-dev bison build-base ccache cmake dev86 flex git lld lzo-dev libtool linux-headers maven ninja python3 zip \
    && ln -s yacc /usr/bin/byacc
COPY . ${BUILD_ROOT}
WORKDIR ${BUILD_ROOT}
RUN --mount=type=cache,target=${BUILD_ROOT}/thirdparty/installed \
    --mount=type=cache,target=${BUILD_ROOT}/thirdparty/src \
    ./thirdparty/build-thirdparty.sh && mkdir -p ${STARROCKS_THIRDPARTY} && cp -r ./thirdparty/installed ${STARROCKS_THIRDPARTY}/


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


FROM builder as datadog-downloader
ARG TARGETARCH
WORKDIR /datadog

# download the latest dd-java-agent
RUN curl -sLo dd-java-agent.jar "https://dtdg.co/latest-java-tracer"

# Get ddprof for BE profiling
RUN curl -sL "https://github.com/DataDog/ddprof/releases/latest/download/ddprof-${TARGETARCH}-linux.tar.xz" | tar -xJv --strip-components 2 ddprof/bin/ddprof


FROM scratch
ARG RELEASE_VERSION
ARG BUILD_ROOT

LABEL org.opencontainers.image.source="https://github.com/starrocks/starrocks"
LABEL org.starrocks.version=${RELEASE_VERSION:-"UNKNOWN"}

COPY --from=fe-builder ${BUILD_ROOT}/output /release/fe_artifacts
COPY --from=be-builder ${BUILD_ROOT}/output /release/be_artifacts
COPY --from=broker-builder ${BUILD_ROOT}/fs_brokers/apache_hdfs_broker/output /release/broker_artifacts
COPY docker/dockerfiles/fe/*.sh /release/fe_k8s
COPY docker/dockerfiles/be/*.sh /release/be_k8s

COPY --from=datadog-downloader /datadog/dd-java-agent.jar /release/fe_artifacts/fe/datadog/dd-java-agent.jar
COPY --from=datadog-downloader /datadog/ddprof /release/be_artifacts/be/datadog/ddprof

WORKDIR /release
