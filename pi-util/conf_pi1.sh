echo "Configure for Pi1"

RPI_BUILDROOT=`pwd`/build
RPI_ROOTFS=$RPI_BUILDROOT/linux/raspian_jessie_pi1-sysroot
RPI_TOOLROOT=$RPI_BUILDROOT/tools/arm-bcm2708/arm-rpi-4.9.3-linux-gnueabihf
RPI_OPT_VC=$RPI_ROOTFS/opt/vc
#RPI_INCLUDES="-I$RPI_OPT_VC/include -I$RPI_ROOTFS/usr/include -I$RPI_OPT_VC/include/interface/vcos/pthreads -I$RPI_OPT_VC/include/interface/vmcs_host/linux"
RPI_INCLUDES="-I$RPI_OPT_VC/include -I$RPI_OPT_VC/include/interface/vcos/pthreads -I$RPI_OPT_VC/include/interface/vmcs_host/linux"
RPI_DEFS="-D__VCCOREVER__=0x04000000 -DRPI=1"
#RPI_DEFS="-D__VCCOREVER__=0x04000000"
RPI_LIBDIRS="-L$RPI_ROOTFS/lib -L$RPI_ROOTFS/usr/lib -L$RPI_OPT_VC/lib"
#RPI_KEEPS="-save-temps=obj"
RPI_KEEPS=""

./configure --enable-cross-compile\
 --cpu=arm1176jzf-s\
 --arch=armv\
 --disable-neon\
 --target-os=linux\
 --disable-stripping\
 --enable-mmal\
 --extra-cflags="-g $RPI_KEEPS $RPI_DEFS $RPI_INCLUDES"\
 --extra-cxxflags="$RPI_DEFS $RPI_INCLUDES"\
 --extra-ldflags="$RPI_LIBDIRS -Wl,-rpath=/opt/vc/lib,-rpath-link=$RPI_OPT_VC/lib,-rpath=/lib,-rpath=/usr/lib,-rpath-link=$RPI_ROOTFS/lib,-rpath-link=$RPI_ROOTFS/usr/lib"\
 --extra-libs="-Wl,--start-group -lbcm_host -lmmal -lmmal_util -lmmal_core -lvcos -lvcsm -lvchostif -lvchiq_arm"\
 --cross-prefix=$RPI_TOOLROOT/bin/arm-linux-gnueabihf-


# --enable-extra-warnings\
# --arch=armv71\
# --enable-shared\

# gcc option for getting asm listing
# -Wa,-ahls
