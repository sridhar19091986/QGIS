#!/bin/sh

arch=${1:-x86_64}
qt=${2:-qt5}

if [ "$arch" == "i686" ]; then
    bits=32
elif [ "$arch" == "x86_64" ]; then
    bits=64
else
    echo "Error: unrecognized architecture $arch"
    exit 1
fi

# Do copies instead of links if building inside container
if [ -f /.dockerenv ]; then
    lnk() {
        cp -a "$1" "$2"
    }
else
    lnk() {
        ln -sf "$1" "$2"
    }
fi

# Note: This script is written to be used with the Fedora mingw environment
MINGWROOT=/usr/$arch-w64-mingw32/sys-root/mingw

optflags="-O2 -g -pipe -Wall -Wp,-D_FORTIFY_SOURCE=2 -fexceptions --param=ssp-buffer-size=4 -fno-omit-frame-pointer"

# Halt on errors
set -e

export MINGW32_CFLAGS="$optflags"
export MINGW32_CXXFLAGS="$optflags"
export MINGW64_CFLAGS="$optflags"
export MINGW64_CXXFLAGS="$optflags"

srcdir="$(readlink -f "$(dirname "$(readlink -f "$0")")/..")"
builddir="$srcdir/build_mingw${bits}_${qt}"
installroot="$builddir/dist"
installprefix="$installroot/usr/$arch-w64-mingw32/sys-root/mingw"

# Cleanup
rm -rf "$installroot"

# Build
mkdir -p $builddir
(
  cd $builddir
  mingw$bits-cmake \
    -DCMAKE_CROSS_COMPILING=1 \
    -DNATIVE_CRSSYNC_BIN=$(readlink -f $srcdir)/build/output/bin/crssync \
    -DQSCINTILLA_VERSION_STR=2.11.1 \
    -DQSCINTILLA_LIBRARY=$MINGWROOT/lib/libqscintilla2_qt5.dll.a \
    -DQSCI_MOD_VERSION_STR=2.11.1 \
    -DQWT_INCLUDE_DIR=$MINGWROOT/include/qt5/qwt \
    -DQSCI_SIP_DIR=$MINGWROOT/share/sip/PyQt5/Qsci/ \
    -DBUILD_TESTING=OFF \
    -DENABLE_TESTS=OFF \
    -DQGIS_BIN_SUBDIR=bin \
    -DQGIS_CGIBIN_SUBDIR=bin \
    -DQGIS_LIB_SUBDIR=lib \
    -DQGIS_LIBEXEC_SUBDIR=lib/qgis \
    -DQGIS_DATA_SUBDIR=share/qgis \
    -DQGIS_PLUGIN_SUBDIR=lib/qgis/plugins \
    -DQGIS_INCLUDE_SUBDIR=include/qgis \
    -DQGIS_QML_SUBDIR=lib/qt5/qml \
    -DBINDINGS_GLOBAL_INSTALL=ON \
    ..
)

# Compile native crssync
# mkdir -p $builddir/native_crssync
# (
# cd $builddir/native_crssync
# echo "Building native crssync..."
# moc-qt5 $srcdir/src/core/qgsapplication.h > moc_qgsapplication.cpp
# g++ $optflags -fPIC -o crssync $srcdir/src/crssync/main.cpp $srcdir/src/crssync/qgscrssync.cpp moc_qgsapplication.cpp $srcdir/src/core/qgsapplication.cpp -DCORE_EXPORT= -DCOMPILING_CRSSYNC -I$srcdir/src/core/ -I$srcdir/src/core/geometry -I$builddir $(pkg-config --cflags --libs Qt5Widgets gdal sqlite3 proj)
# )
# crssync needs X at runtime
# Xvfb :99 &
# export DISPLAY=:99

njobs=$(($(grep -c ^processor /proc/cpuinfo) * 3 / 2))
mingw$bits-make -C$builddir -j$njobs DESTDIR="${installroot}" install

binaries=$(find $installprefix -name '*.exe' -or -name '*.dll' -or -name '*.pyd')

# Strip debuginfo
for f in $binaries
do
    case $(mingw-objdump -h $f 2>/dev/null | egrep -o '(debug[\.a-z_]*|gnu.version)') in
        *debuglink*) continue ;;
        *debug*) ;;
        *gnu.version*)
        echo "WARNING: $(basename $f) is already stripped!"
        continue
        ;;
        *) continue ;;
    esac

    echo extracting debug info from $f
    mingw-objcopy --only-keep-debug $f $f.debug || :
    pushd $(dirname $f)
    keep_symbols=`mktemp`
    mingw-nm $f.debug --format=sysv --defined-only | awk -F \| '{ if ($4 ~ "Function") print $1 }' | sort > "$keep_symbols"
    mingw-objcopy --add-gnu-debuglink=`basename $f.debug` --strip-unneeded `basename $f` --keep-symbols="$keep_symbols" || :
    rm -f "$keep_symbols"
    popd
done

# Collect dependencies
function isnativedll {
    # If the import library exists but not the dynamic library, the dll ist most likely a native one
    local lower=${1,,}
    [ ! -e $MINGWROOT/bin/$1 ] && [ -f $MINGWROOT/lib/lib${lower/%.*/.a} ] && return 0;
    return 1;
}

function linkDep {
# Link the specified binary dependency and it's dependencies
    local indent=$3
    local destdir="$installprefix/${2:-bin}"
    local name="$(basename $1)"
    test -e "$destdir/$name" && return 0
    test -e "$destdir/qgisplugins/$name" && return 0
    echo "${indent}${1}"
    [ ! -e "$MINGWROOT/$1" ] && (echo "Error: missing $MINGWROOT/$1"; return 1)
    mkdir -p "$destdir" || return 1
    lnk "$MINGWROOT/$1" "$destdir/$name" || return 1
    echo "${2:-bin}/$name: $(rpm -qf "$MINGWROOT/$1")" >> $installprefix/origins.txt
    autoLinkDeps "$destdir/$name" "${indent}  " || return 1
    [ -e "$MINGWROOT/$1.debug" ] && lnk "$MINGWROOT/$1.debug" "$destdir/$name.debug" || echo "Warning: missing $name.debug"
    return 0
}

function autoLinkDeps {
# Collects and links the dependencies of the specified binary
    for dep in $(mingw-objdump -p "$1" | grep "DLL Name" | awk '{print $3}'); do
        if ! isnativedll "$dep"; then
            # HACK fix incorrect libpq case
            dep=${dep/LIBPQ/libpq}
            linkDep bin/$dep bin "$2" || return 1
        fi
    done
    return 0
}

echo "Linking dependencies..."
for binary in $binaries; do
    autoLinkDeps $binary
done
linkDep bin/gdb.exe
linkDep bin/python3.exe
linkDep bin/python3w.exe

linkDep $(ls $MINGWROOT/bin/libssl-*.dll | sed "s|$MINGWROOT/||")
linkDep $(ls $MINGWROOT/bin/libcrypto-*.dll | sed "s|$MINGWROOT/||")
linkDep lib/mod_spatialite.dll bin

# Additional dependencies
linkDep lib/qt5/plugins/imageformats/qgif.dll  bin/imageformats
linkDep lib/qt5/plugins/imageformats/qicns.dll bin/imageformats
linkDep lib/qt5/plugins/imageformats/qico.dll  bin/imageformats
linkDep lib/qt5/plugins/imageformats/qjp2.dll  bin/imageformats
linkDep lib/qt5/plugins/imageformats/qjpeg.dll bin/imageformats
linkDep lib/qt5/plugins/imageformats/qtga.dll  bin/imageformats
linkDep lib/qt5/plugins/imageformats/qtiff.dll bin/imageformats
linkDep lib/qt5/plugins/imageformats/qwbmp.dll bin/imageformats
linkDep lib/qt5/plugins/imageformats/qwebp.dll bin/imageformats
linkDep lib/qt5/plugins/imageformats/qsvg.dll  bin/imageformats
linkDep lib/qt5/plugins/platforms/qwindows.dll bin/platforms
linkDep lib/qt5/plugins/printsupport/windowsprintersupport.dll bin/printsupport
linkDep lib/qt5/plugins/styles/qwindowsvistastyle.dll bin/styles
linkDep lib/qt5/plugins/audio/qtaudio_windows.dll bin/audio
linkDep lib/qt5/plugins/mediaservice/dsengine.dll bin/mediaservice
linkDep lib/qt5/plugins/mediaservice/qtmedia_audioengine.dll bin/mediaservice
linkDep lib/qt5/plugins/sqldrivers/qsqlite.dll bin/sqldrivers
linkDep lib/qt5/plugins/sqldrivers/qsqlodbc.dll bin/sqldrivers
linkDep lib/qt5/plugins/sqldrivers/qsqlpsql.dll bin/sqldrivers

linkDep lib/qt5/plugins/crypto/libqca-gcrypt.dll bin/crypto
linkDep lib/qt5/plugins/crypto/libqca-logger.dll bin/crypto
linkDep lib/qt5/plugins/crypto/libqca-softstore.dll bin/crypto
linkDep lib/qt5/plugins/crypto/libqca-gnupg.dll bin/crypto
linkDep lib/qt5/plugins/crypto/libqca-ossl.dll bin/crypto

mkdir -p $installprefix/share/qt5/translations/
cp -a $MINGWROOT/share/qt5/translations/qt_*.qm  $installprefix/share/qt5/translations
cp -a $MINGWROOT/share/qt5/translations/qtbase_*.qm  $installprefix/share/qt5/translations

# Install python libs
(
cd $MINGWROOT
SAVEIFS=$IFS
IFS=$(echo -en "\n\b")
for file in $(find lib/python3.7 -type f); do
    mkdir -p "$installprefix/$(dirname $file)"
    lnk "$MINGWROOT/$file" "$installprefix/$file"
done
IFS=$SAVEIFS
)

# Osg plugins
osgPlugins=$(basename $MINGWROOT/bin/osgPlugins-*)
lnk $MINGWROOT/bin/$osgPlugins $installprefix/bin/$osgPlugins

# Data files
mkdir -p $installprefix/share/
lnk /usr/share/gdal $installprefix/share/gdal

# Sort origins file
cat $installprefix/origins.txt | sort | uniq > $installprefix/origins.new && mv $installprefix/origins.new $installprefix/origins.txt
