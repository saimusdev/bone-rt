# [Beaglebone](http://beagleboard.org/bone) Time Slice monitoring

This is a hacked real-time Linux kernel for monitoring of the Time Slice of particular 
processes (for educational purposes only). The kernel is based on the PREEMPT_RT patch (v3.14.29-rt26)

## Preamble

A script is provided to help with the build process. The script consists of several tasks,
which can be run independently, that is, without the need of starting the whole process again. 

You'll simply ought to run the **bone.sh** program with different arguments. 
The possible command line options are:

- **build** - Builds the Kernel and all necessary files.
- **rebuild** - Rebuilds the Kernel (only compiling, linking and so on the modified files).
- **clean** - Cleans the Kernel source tree from built files.
- **format** - Formats and creates the appropiate partitions on the (connected) Beaglebone SD card.
- **install** - Downloads and transfers all the necessary files to the (connected) Beaglebone SD card
in order to make a bootable Linux.
- **update** - Transfers only the updated (modified) files to the SD card. Usually this means
transfering the kernel image only.
- **help** - Prints a help menu

## Build process

In this section, the process of building the modified kernel is described (using the previously mentioned script).

#### 0 - Dependencies

These are the basic dependencies for the build process:

- Build Essentials (build-essential)
- git
- lzop
- Universal Boot Loader (u-boot-tools)
- ncurses library (lib32ncurses5-dev)

also, you'll need the cross compiling tool:

- [Linaro Toolchain for Cortex-A](https://wiki.linaro.org/WorkingGroups/ToolChain): more precisely, the little endian hard float version (**arm-linux-gnueabihf >=4.8**) of this toolchain. Note: Ubuntu machines (>10.10) already include the toolchain: ```apt-get install gcc-arm-linux-gnueabihf```

#### 1 - BUILD

Next comes the process of building the kernel image. Simply run the following:

```
./bone.sh build
```
#### 2 - FORMAT

Thirdly, the Beaglebone SD card has to be formatted in a specific manner:

```
./bone.sh format
```


#### 3 - INSTALL

Finally, after the image and all necessary files have been built, you'll need to transfer them to the Beaglebone (the connected SD card):

```
./bone.sh install
```
