# 野火imx6ull系列开发板(u-boot)

## 开发环境

**ubuntu18.04**

# 野火imx6ull系列开发板(u-boot)

## 开发环境

**ubuntu18.04**

安装依赖工具：
```
sudo apt install make gcc-arm-linux-gnueabihf gcc bison flex libssl-dev dpkg-dev lzop
```

**测试arm-none-eabi-gcc安装是否成功**

```bash
arm-none-eabi-gcc -v
```

**如果你的系统是64位的**

如果出现`No such file or directory`问题，可以用以下命令解决
```bash
sudo apt-get install lib32ncurses5 lib32tinfo5 libc6-i386
```
---

## 编译过程

**清除编译信息**

```bash
make ARCH=arm clean
```

## 设置配置选项

选择一个与野火开发板版本一致的配置即可!!

> 注意：SD卡、eMMC共用同一版本配置，此时u-boot的启动顺序为：->SD卡->eMMC

**nand版本**

```bash
make ARCH=arm mx6ull_fire_nand_defconfig
```

**sd卡/emmc版本**

```bash
make ARCH=arm mx6ull_fire_mmc_defconfig
```

**开始编译**
```bash
make -j4 ARCH=arm CROSS_COMPILE=arm-none-eabi-
```

## 编译生成的uboot输出路径

生成`u-boot-dtb.imx`文件

```bash
u-boot/u-boot-dtb.imx
```

---

## 其他信息

**u-boot版本信息**

```
git clone from https://github.com/u-boot/u-boot.git 
```

使用2020.10版本

已适配野火imx6ull系列开发板














**测试arm-none-eabi-gcc安装是否成功**

```bash
arm-none-eabi-gcc -v
```

**如果你的系统是64位的**

如果出现`No such file or directory`问题，可以用以下命令解决
```bash
sudo apt-get install lib32ncurses5 lib32tinfo5 libc6-i386
```
---

## 编译过程

**清除编译信息**

```bash
make ARCH=arm clean
```

## 设置配置选项

选择一个与野火开发板版本一致的配置即可!!

> 注意：SD卡、eMMC共用同一版本配置，此时u-boot的启动顺序为：->SD卡->eMMC

**nand版本**

```bash
make ARCH=arm mx6ull_fire_nand_defconfig
```

**sd卡/emmc版本**

```bash
make ARCH=arm mx6ull_fire_mmc_defconfig
```

**开始编译**
```bash
make -j4 ARCH=arm CROSS_COMPILE=arm-none-eabi-
```

## 编译生成的uboot输出路径

生成`u-boot-dtb.imx`文件

```bash
u-boot/u-boot-dtb.imx
```

---

## 其他信息

**u-boot版本信息**

```
git clone from https://github.com/u-boot/u-boot.git 
``

使用2020.10版本
 
已适配野火imx6ull系列开发板












