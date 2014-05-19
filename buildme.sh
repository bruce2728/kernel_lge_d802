#!/bin/bash

KERNELDIR=~/kernel-build;
export CROSS_COMPILE=~/arm-linux-androideabi-4.8-linaro/bin/arm-eabi-
export PATH=$PATH:tools/lz4demo

# clean the build dir
make mrproper
rm "$KERNELDIR"/out/*.zip
rm "$KERNELDIR"/out/*.img

# make defconfig and zimage
make d802_defconfig
make -j4

# make modules and move to out dir
make modules -j4
for i in $(find "$KERNELDIR" -name '*.ko'); do
        cp -av "$i" "$KERNELDIR"/out/system/lib/modules/;
done;
chmod 644 "$KERNELDIR"/out/system/lib/modules/*

# move zImage
mkdir "$KERNELDIR"/out/temp
cp arch/arm/boot/zImage "$KERNELDIR"/out/temp/zImage

# compress ramdisk
scripts/mkbootfs "$KERNELDIR"/ramdisk | gzip > ramdisk.gz 2>/dev/null
mv ramdisk.gz "$KERNELDIR"/out/temp/

# run dtbtool
./scripts/dtbTool -v -s 2048 -o "$KERNELDIR"/out/temp/dt.img arch/arm/boot/

# make boot.img
cp scripts/mkbootimg "$KERNELDIR"/out/temp
cd "$KERNELDIR"/out/temp

./mkbootimg --kernel zImage --ramdisk ramdisk.gz --cmdline "console=ttyHSL0,115200,n8 androidboot.hardware=g2 user_debug=31 msm_rtb.filter=0x0 mdss_mdp.panel=1:dsi:0:qcom,mdss_dsi_g2_lgd_cmd" --base 0x00000000 --offset 0x05000000 --tags-addr 0x04800000 --pagesize 2048 --dt dt.img -o boot.img

cp boot.img "$KERNELDIR"/out/boot.img

# delete temp files
cd "$KERNELDIR"/out
rm -rf temp

# make flashable zip
zip -r Kernel-bruce2728-D802.zip * >/dev/null
cd "$KERNELDIR"

