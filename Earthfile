VERSION 0.6
FROM docker.io/library/debian:bullseye
WORKDIR /workdir

install:
    RUN apt-get update \
     && apt-get upgrade -y \
     && apt-get install --no-install-recommends -y build-essential debhelper \
        devscripts python3-pip ninja-build g++ liblua5.1-0-dev libgit2-dev \
        libzip-dev zlib1g-dev pkg-config
    RUN pip install 'cmake==3.25.*'

build:
    ARG VERSION=0.0.0
    FROM +install
    RUN mkdir /build
    COPY . .
    RUN cmake -S . -B /build \
        -DBUILD_SHARED_LIBS=YES \
        -DRAPIDTOOLS_VERSION=$VERSION \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -G Ninja
    RUN cmake --build /build
    RUN cd /build && cpack -G DEB
    SAVE ARTIFACT /build/*deb AS LOCAL out/
