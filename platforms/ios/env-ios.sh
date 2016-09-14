#!/bin/bash

#Source this file in the current shell to setup the cross-compile build environment

if (( "$#" < 1 )); then
    echo -e "\n\
Usage: env-ios.sh <prefix-dir>\n\
       Where prefix-dir is the prefix (a directory containing include\n\
       and lib subdirectories) for building and searching for non-system\n\
       libraries. This is in contrast with the sysroot, which is provided by
       the iOS SDK, is detected automatically, and provides the SDK's system
       headers and libraries"
    return 1
fi

if [ ! -d "$1" ]; then
    echo "Specified prefix directory '$1' does not exist"
   return 1
fi

# Envirnoment variables:

#=== User-set variables
# Set IOSC_TARGET env var to the either iphoneos or iphonesimulator.
# It will be passed as the -sdk parameter to xcrun

if [ -z "$IOSC_TARGET" ]; then
    export IOSC_TARGET=iphoneos
fi
#=== End of user-set variables

export IOSC_PREFIX="$1"
owndir=`echo "$(cd $(dirname "${BASH_SOURCE[0]}"); pwd)"`

if [ "$IOSC_TARGET" == "iphoneos" ]; then
    export IOSC_ARCH=armv7
    export IOSC_OS_VERSION=-mios-version-min=6.0

#suitable for --host= parameter to configure
    export IOSC_HOST_TRIPLET=arm-apple-darwin11
    export IOSC_PLATFORM_SDKNAME=iPhoneOS
else
    export IOSC_ARCH=x86_64
    export IOSC_OS_VERSION=
    export IOSC_HOST_TRIPLET=
    export IOSC_PLATFORM_SDKNAME=iPhoneSimiulator
fi

export IOSC_CMAKE_TOOLCHAIN="$owndir/ios.$IOSC_TARGET.toolchain.cmake"
export IOSC_SYSROOT=`xcrun -sdk $IOSC_TARGET -show-sdk-path`

find="xcrun -sdk $IOSC_TARGET -find"
compileropts="-arch $IOSC_ARCH $IOSC_OS_VERSION -stdlib=libc++"

export LDFLAGS="--sysroot $IOSC_SYSROOT -L$IOSC_PREFIX/lib $compileropts"
export CFLAGS="$compileropts"
export CXXFLAGS="$compileropts"
export CPPFLAGS="-isysroot$IOSC_SYSROOT -I$IOSC_PREFIX/include -arch $IOSC_ARCH"

export CC="`$find clang`"
export CXX="`$find clang++`"
export CPP="$CC -E"
export LD="`$find clang++`"
export AR=`$find ar`
export LIBTOOL=`$find libtool`
export RANLIB=`$find ranlib`
export AS=`$find as`
export STRIP=`$find strip`

# Convenience variables
# CMake command to configure strophe build to use the android toolchain:
export CMAKE_XCOMPILE_ARGS="-DCMAKE_TOOLCHAIN_FILE=$IOSC_CMAKE_TOOLCHAIN -DCMAKE_INSTALL_PREFIX=$IOSC_PREFIX"


# Typical configure command to build dependencies:
export CONFIGURE_XCOMPILE_ARGS="--prefix=$IOSC_PREFIX --host=$IOSC_HOST_TRIPLET"

function xcmake
{
    cmake $CMAKE_XCOMPILE_ARGS $@
}

function xconfigure
{
   ./configure $CONFIGURE_XCOMPILE_ARGS $@
}
export -f xcmake
export -f xconfigure

echo "============================================"
echo "Envirnoment set to use the following compilers:"
echo "CC=$CC"
echo "CXX=$CXX"
echo "SYSROOT(sdk root)=$IOSC_SYSROOT"
echo "PREFIX(user libs and headers)=$IOSC_PREFIX"
echo
echo -e "You can use\n\
xconfigure [your-args]\n\
or\n\
\033[1;31meval\033[0m ./configure \$CONFIGURE_XCOMPILE_ARGS [your-args]\n\
to run configure scripts. This also sets up the install prefix to the PREFIX directory"
echo
echo -e "You can use\n\
xcmake [your-args]\n\
or\n\
\033[1;31meval\033[0m cmake \$CMAKE_XCOMPILE_ARGS [your-args]\n\
to run a CMake command. This also sets up the install prefix to the PREFIX directory"
