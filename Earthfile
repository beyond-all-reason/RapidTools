VERSION 0.8

install:
    ARG DEBIAN_VERSION
    FROM docker.io/library/debian:$DEBIAN_VERSION
    WORKDIR /workdir
    RUN apt-get update \
     && apt-get upgrade -y \
     && apt-get install --no-install-recommends -y build-essential debhelper \
        devscripts ninja-build g++ liblua5.1-0-dev libgit2-dev \
        libzip-dev zlib1g-dev pkg-config cmake

build:
    ARG DEBIAN_VERSION
    ARG VERSION=0.0.0
    FROM +install
    RUN mkdir /build
    COPY . .
    RUN cmake -S . -B /build \
        -DBUILD_SHARED_LIBS=YES \
        -DRAPIDTOOLS_VERSION=$VERSION \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCPACK_DEBIAN_PACKAGE_RELEASE=0+debian${DEBIAN_VERSION} \
        -G Ninja
    RUN cmake --build /build
    RUN cd /build && cpack -G DEB
    SAVE ARTIFACT /build/*deb AS LOCAL out/
