#!/bin/bash
#设置环境

KERNEL_PATH=$PWD

SETUP_KERNELSU=true
KERNELSU_TAG=main

# Special Clean For Huawei Kernel.
if [ -d include/config ];
then
    echo "Find config,will remove it"
	rm -rf include/config
else
	echo "No Config,good."
fi

echo " "
echo "***Setting environment...***"
# 交叉编译器路径
export PATH=$PATH:$KERNEL_PATH/aarch64-linux-android-4.9/bin
export CROSS_COMPILE=aarch64-linux-android-

export GCC_COLORS=auto
export ARCH=arm64

if [ "$1" == "clean" ]; then
	rm -rf ./out
fi

if [ ! -d "out" ];
then
	mkdir out
fi

date="$(date +%Y.%m.%d-%I:%M)"

# 配置KernelSU
if [ "$SETUP_KERNELSU" == "true" ];
then
	curl -LSs "https://raw.githubusercontent.com/tiann/KernelSU/main/kernel/setup.sh" | bash -s "$KERNELSU_TAG"
else
	test -d "$KERNEL_PATH/KernelSU" && rm -rf "$KERNEL_PATH/KernelSU"
	test -e "$KERNEL_PATH/drivers/kernelsu" && rm "$KERNEL_PATH/drivers/kernelsu"
	grep -q "kernelsu" "$KERNEL_PATH/drivers/Makefile" && sed -i '/kernelsu/d' "$KERNEL_PATH/drivers/Makefile"
	grep -q "kernelsu" "$KERNEL_PATH/drivers/Kconfig" && sed -i '/kernelsu/d' "$KERNEL_PATH/drivers/Kconfig"
fi

#构建内核部分
echo "***Building Kernel...***"
make ARCH=arm64 O=out bach_defconfig
# 定义编译线程数
make ARCH=arm64 O=out -j`nproc` 2>&1 | tee kernel_log-${date}.txt
