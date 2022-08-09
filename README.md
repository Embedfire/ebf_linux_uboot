# 野火STM32MP157 pro开发板(u-boot)

## 开发环境

**ubuntu18.04**

安装依赖工具：
```
sudo apt install make gcc-arm-none-eabi gcc bison flex libssl-dev dpkg-dev lzop libncurses5-dev libncursesw5-dev
```

**测试arm-none-eabi-gcc安装是否成功**

```bash
arm-none-eabi-gcc -v
```

## 编译过程

选择野火开发板版本配置即可

> 注意：SD卡、eMMC共用同一版本配置

**配置**

```bash
make stm32mp15_trusted_defconfig
```

**开始编译**
```bash
make CROSS_COMPILE=arm-none-eabi- DEVICE_TREE=stm32mp157a-star all
```

**编译生成的uboot输出路径**

生成带image header的`u-boot.stm32`文件

```bash
ebf_linux_uboot/u-boot.stm32
```

**清除编译输出**

```bash
make clean
```
---

## 其他信息

