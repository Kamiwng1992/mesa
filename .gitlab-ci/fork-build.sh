#!/bin/bash

set -xe

export DEBIAN_FRONTEND=noninteractive

if ! grep -q universe /etc/apt/sources.list; then
    sed -i 's/main/main universe/' /etc/apt/sources.list
fi
if ! grep -q ports.ubuntu.com /etc/apt/sources.list; then
    sed -i '/^deb http/s@ \([^ ]*\) \(.*\)@ [arch=amd64,i386] \1 \2\ndeb [arch=arm64,armhf] http://ports.ubuntu.com \2@' /etc/apt/sources.list
    cat /etc/apt/sources.list
fi

dpkg --add-architecture $arch

apt-get update

mkdir -pv apt-cache

apt-get -o dir::cache::archives="$(pwd)"/apt-cache install -y --no-remove --no-install-recommends \
        wget \
        meson \
        bison \
        flex \
        git \
        ccache \
        ca-certificates \
        build-essential \
        pkg-config \
        python3-setuptools \
        python3-mako \
        libwayland-dev \
        software-properties-common \
        crossbuild-essential-$arch \
        libelf-dev:$arch \
        libexpat1-dev:$arch \
        libpciaccess-dev:$arch \
        libstdc++6:$arch \
        libvulkan-dev:$arch \
        libx11-dev:$arch \
        libx11-xcb-dev:$arch \
        libxcb-dri2-0-dev:$arch \
        libxcb-dri3-dev:$arch \
        libxcb-glx0-dev:$arch \
        libxcb-present-dev:$arch \
        libxcb-randr0-dev:$arch \
        libxcb-shm0-dev:$arch \
        libxcb-xfixes0-dev:$arch \
        libxdamage-dev:$arch \
        libxext-dev:$arch \
        libxrandr-dev:$arch \
        libxshmfence-dev:$arch \
        libxxf86vm-dev:$arch \
        libsensors-dev:$arch \
        libzstd-dev:$arch \
        libdw-dev:$arch \
        libwayland-dev:$arch \
        libwayland-egl-backend-dev:$arch

add-apt-repository -y ppa:oibaf/graphics-drivers

apt-get update
apt-get -o dir::cache::archives="$(pwd)"/apt-cache install -y --no-remove --no-install-recommends wayland-protocols libdrm-dev:$arch

sed -i 's/-ldrm/-l:libdrm.a -lm/' /usr/lib/*/pkgconfig/libdrm.pc

export CCACHE_BASEDIR="$(pwd)"
export CCACHE_DIR="$(pwd)/ccache" && mkdir -pv "$CCACHE_DIR"
ccache -z -M 500M

if [ "$arch" = armhf ]; then
    CFLAGS='-marm -march=armv7ve -mfpu=neon-vfpv4 -mtune=cortex-a72'
elif [ "$arch" = arm64 ]; then
    CFLAGS='-mno-outline-atomics -mtune=cortex-a72'
fi

CFLAGS=$CFLAGS CXXFLAGS=$CFLAGS LDFLAGS=$CFLAGS . .gitlab-ci/container/create-cross-file.sh $arch

EXTRA_MESON_ARGS="--cross-file=/cross_file-${arch}.txt -D libdir=lib/$(dpkg-architecture -A $arch -qDEB_TARGET_MULTIARCH)"

rm -rf _build
mkdir _build
cd _build

# Use a long string that can be patched by the installer later
DRI_DRIVERS_PATH=DRI-DRIVERS-PATH0123456789112345678921234567893123456789412345678951234567896123456789712345678981234567899123456789DRI-DRIVERS-PATH-END

sed -i "s/flto'/flto=$(nproc)'/" /usr/lib/python3/dist-packages/mesonbuild/linkers.py

# TODO: Perfetto support

meson -Dplatforms=x11,wayland -Ddri-drivers= -Dgallium-drivers=panfrost,swrast -Ddri-search-path=$DRI_DRIVERS_PATH -Dvulkan-drivers=panfrost -Dglvnd=false -Dllvm=disabled -Dlibunwind=disabled -Dzstd=enabled --buildtype=release -Db_lto=true -Dgallium-nine=true -Dd3d-drivers-path=/usr/local/lib/d3d $EXTRA_MESON_ARGS

DESTDIR=_inst ninja install

mkdir panfork
mv _inst/usr/local/* panfork
mv panfork/lib/*/* panfork
rmdir panfork/lib/* panfork/lib

touch panfork/"$(git log -1 | sed -n '1s/commit \([^ ]\)\( .*\)\?/\1/p')"

tar cvJf ../panfork.tar.xz panfork
sha256sum ../panfork.tar.xz
ccache -s
