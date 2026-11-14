FROM gcr.io/oss-fuzz-base/base-builder:ubuntu-24-04
# The packages of the Linux CI build (.github/workflows/c-cpp.yml): Arrow from
# its apt source, LLVM 22 from apt.llvm.org, and CMake >= 3.30.
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates g++-14 libpcre2-dev \
        libreadline-dev lsb-release ninja-build patchelf pkg-config wget zip \
    && wget -q https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-noble.deb \
    && apt-get install -y ./apache-arrow-apt-source-latest-noble.deb \
    && rm apache-arrow-apt-source-latest-noble.deb \
    && wget -qO /etc/apt/trusted.gpg.d/apt.llvm.org.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
    && echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-22 main" > /etc/apt/sources.list.d/llvm-22.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends libarrow-dev libparquet-dev llvm-22-dev \
    && wget -q https://github.com/Kitware/CMake/releases/download/v3.31.9/cmake-3.31.9-linux-x86_64.sh \
    && sh cmake-3.31.9-linux-x86_64.sh --skip-license --prefix=/usr/local \
    && rm cmake-3.31.9-linux-x86_64.sh
COPY . $SRC/qumirdb
WORKDIR $SRC/qumirdb
# ClusterFuzzLite clones the repository without submodules.
RUN git submodule update --init externals/qumir
COPY .clusterfuzzlite/build.sh $SRC/
