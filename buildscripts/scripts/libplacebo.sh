#!/bin/bash -e

. ../../include/path.sh

build=_build$ndk_suffix

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf $build
	exit 0
else
	exit 255
fi

# Android provides Vulkan, but no pkgconfig file, and -Dvk-proc-addr=enabled is
# a hard requirement: without this, `dependency('vulkan')` resolves to NO and
# meson fails with "Feature vk-proc-addr cannot be enabled".
# scripts/mpv.sh writes the same file, but libplacebo is built *before* mpv
# (dep_mpv=(ffmpeg libass lua libplacebo)), so it has to be created here too.
# You can double-check the version in vulkan_core.h (-> VK_HEADER_VERSION).
mkdir -p "$prefix_dir"/lib/pkgconfig
cat >"$prefix_dir"/lib/pkgconfig/vulkan.pc <<"END"
Name: Vulkan
Description:
Version: 1.3.275
Libs: -lvulkan
Cflags:
END

unset CC CXX
meson setup $build --cross-file "$prefix_dir"/crossfile.txt \
	-Dvk-proc-addr=enabled -Ddemos=false

ninja -C $build -j$cores
DESTDIR="$prefix_dir" ninja -C $build install

# add missing library for static linking
# this isn't "-lstdc++" due to a meson bug: https://github.com/mesonbuild/meson/issues/11300
${SED:-sed} '/^Libs:/ s|$| -lc++|' "$prefix_dir/lib/pkgconfig/libplacebo.pc" -i
