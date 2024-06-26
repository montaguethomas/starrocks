# Build toolchains on alpine3.20, dev-env image can be built based on this image for alpine3.20
#  DOCKER_BUILDKIT=1 docker build --rm=true -f docker/dockerfiles/toolchains/toolchains-alpine.Dockerfile -t toolchains-alpine:latest docker/dockerfiles/toolchains/

FROM alpine/java:11-jdk

# Common Packages used in final images
RUN apk add --no-cache bash curl libc6-compat mysql-client openjdk8-jre-base

# Build packages
# byacc conflicts with bison due to both providing /usr/bin/yacc. byacc is just a symlink to yacc and bison's yacc is suppose to be compatible.
RUN apk add --no-cache abseil-cpp-dev autoconf automake binutils-dev bison build-base ccache cmake dev86 flex git lld lzo-dev libtool linux-headers maven ninja python3 zip \
    && ln -s yacc /usr/bin/byacc
