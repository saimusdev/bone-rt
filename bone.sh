#!/bin/bash
#set -x # verbose

if [ $# -ne 1 ]; then
	echo "usage: `basename $0` command"
	exit 1
else
	case "$1" in
		build)
			# number of machine processors x2 (for better performance during kernel build)
			num_proc=`cat /proc/cpuinfo | grep processor | wc -l`
			num_jobs=$(( $num_proc * 2 ))
			echo "using all ${num_proc} available processors in the machine"	

			# Linaro toolchain gcc prefix
			export CC=/usr/bin/arm-linux-gnueabihf-
			kernel_version="3.8.13-rt14"
			make="make ARCH=arm CROSS_COMPILE=${CC} -j${num_jobs}"
			
			# Enter Kernel sources dir
			cd kernel

			echo "using beaglebone_defconfig..."
			$make beaglebone_defconfig
			
			echo "now, tweak the kernel to your liking"
			$make menuconfig

			echo "building $kernel_version kernel image.."
			$make uImage dtbs

			echo "building kernel modules.."
			$make modules
			
			echo "installing kernel and modules.."
			$make INSTALL_MOD_PATH=$PWD/rootfs modules_install

			echo "building Beaglebone kernel image.."
			$make uImage-dtb.am335x-bone
			;;

		rebuild)
			diff -q 2>/dev/null kernel/.config kernel/arch/arm/configs/beaglebone_defconfig
			if [ $? -eq 0 ]; then
				# number of machine processors x2 (for better performance during kernel build)
				num_proc=`cat /proc/cpuinfo | grep processor | wc -l`
				num_jobs=$(( $num_proc * 2 ))
				echo "using all ${num_proc} available processors in the machine"	

				# Linaro toolchain gcc prefix
				export CC=/usr/bin/arm-linux-gnueabihf-
				kernel_version="3.8.13-rt14"
				make="make ARCH=arm CROSS_COMPILE=${CC}"

				# Enter Kernel sources dir
				cd kernel

				echo "building $kernel_version kernel image.."
				$make uImage dtbs

				echo "building kernel modules.."
				$make modules
				
				echo "installing kernel and modules.."
				$make INSTALL_MOD_PATH=$PWD/rootfs modules_install

				echo "building Beaglebone kernel image.."
				$make uImage-dtb.am335x-bone

			else
				echo "run first: `basename $0` build"
				echo "this builds all the necessary files, not just the ones modified"
			fi
			;;

		clean)
			# Enter Kernel sources dir
			cd kernel

			# Linaro toolchain gcc prefix
			export CC=/usr/bin/arm-linux-gnueabihf-
			make="make ARCH=arm CROSS_COMPILE=${CC}"

			$make distclean
			$make clean
			$make mrproper
			;;

		format)

			if [ "$EUID" -ne 0 ]; then
				echo "Please run as root"
			else
				echo "-- LIST OF CONNECTED DRIVES ON THE SYSTEM --"
				lsblk

				read -p "Which drive corresponds to the SD card (/dev/sdX): " sdcard
				case $sdcard in
					/dev/sd* )
						test ! -b $sdcard && echo "Drive does not exist!" && exit
						echo "This could potentially wipe your Hard Drive!"
						while true; do
							read -p "Are you sure you want to format drive [Y/N]? " answer
							case $answer in
								[Yy]* ) break;;
								[Nn]* ) exit;;
								* ) echo "Please answer yes or no.";;
							esac
						done;;
					* )
						echo "Wrong drive name!"
						exit;;
				esac

				while true; do
						read -p "Really sure [Y/N]? " answer
						case $answer in
								[Yy]* ) break;;
								[Nn]* ) exit;;
								* ) echo "Please answer yes or no.";;
						esac
				done

				#echo "zeroing sd card..."
				#shred -zn 2 $sdcard

				echo "creating partition table..."
				cat <<EOF | fdisk $sdcard
o
p
n
p
1

+64M
t
e
a
1
n
p
2


w
EOF
				echo "formating partitions..."
				mkfs.vfat -F 16 ${sdcard}1 -n BOOT
				mkfs.ext4 ${sdcard}2 -L ROOTFS
			fi
			;;

		install)
			if [ "$EUID" -ne 0 ]; then
				echo "Please run as root"
			else
				# specify mount points for beaglebone sd card
				boot=/mnt/beagleboot
				rootfs=/mnt/beaglerootfs

				# select the drive which corresponds to the sd card
				echo "-- LIST OF CONNECTED DRIVES ON THE SYSTEM --"
				lsblk
				read -p "Which drive corresponds to the SD card (/dev/sdX): " sdcard
				case $sdcard in
					/dev/sd* )
						test ! -b $sdcard && echo "Drive does not exist!" && exit;;
					* )
						echo "Wrong drive name!" && exit;;
				esac

				# create (if nonexistent) mountpoints
				test ! -d $boot && mkdir $boot
				test ! -d $rootfs && mkdir $rootfs

				# mount beaglebone partitions
				echo "mounting sd card partitions..."
				mountpoint -q $boot
				if [ $? -eq 0 ]; then
					echo "$boot already mounted!"
					exit
				else
					mount ${sdcard}1 $boot
				fi

				mountpoint -q $rootfs
				if [ $? -eq 0 ]; then
					echo "$rootfs already mounted!"
					exit
				else
					mount ${sdcard}2 $rootfs
				fi

				# Save current working directory
				wd=`pwd`

				# Install bootloader
				echo "installing booloader..."
				wget -q "http://archlinuxarm.org/os/omap/BeagleBone-bootloader.tar.gz"
				tar -xvzf BeagleBone-bootloader.tar.gz -C $boot
				rm -f BeagleBone-bootloader.tar.gz

				# umount boot partition
				umount $boot && rm -rf $boot

				# Create filesystem tree
				echo "creating filesystem tree..."
				mkdir -p ${rootfs}/{boot/dtbs,usr/bin,dev,proc,sys,tmp,etc/init.d,etc/selinux}
				cd $rootfs
				echo "SELINUX=disabled" > etc/selinux/config
				ln -s usr/bin ${rootfs}/bin
				ln -s usr/bin ${rootfs}/sbin

				# Install busybox and init
				echo "installing busybox..."
				wget -q "http://busybox.net/downloads/binaries/1.21.1/busybox-armv7l" -O ${rootfs}/usr/bin/busybox
				cd ${rootfs}/usr/bin
				ln -s busybox ${rootfs}/sbin/init
				chmod 755 busybox

				cat <<EOF > ${rootfs}/etc/init.d/rcS
#!/usr/bin/busybox ash

# Installing links only really necessary the first time

/usr/bin/busybox mount -t proc none /proc
/usr/bin/busybox mount -t sysfs none /sys
/usr/bin/busybox mount -o remount,rw /

/usr/bin/busybox --install -s /usr/bin

# If u-boot boots in read-only mode
#/usr/bin/mount -o remount,rw /

/bin/ash
EOF

				chmod +x ${rootfs}/etc/init.d/rcS

				echo "copying linux files..."
				cp='rsync -a --progress'

				cd ${wd}/kernel
				$cp arch/arm/boot/zImage ${rootfs}/boot
				$cp arch/arm/boot/dts/am335x-bone-common.dtsi ${rootfs}/boot/dtbs
				$cp arch/arm/boot/dts/am335x-bone.dts ${rootfs}/boot/dtbs
				$cp arch/arm/boot/dts/am335x-bone.dtb ${rootfs}/boot/dtbs
				$cp -r rootfs/lib ${rootfs}

				# set permissions
				chown -R root:root ${rootfs}/lib
				chown -R root:root ${rootfs}/boot

				# umount root partition
				umount $rootfs && rm -rf $rootfs

				echo "Done! Extract the SD card"
			fi
			;;

		update)
			if [ "$EUID" -ne 0 ]; then
				echo "Please run as root"
			else
				# specify mount points for beaglebone sd card
				rootfs=/mnt/beaglerootfs

				# select the drive which corresponds to the sd card
				echo "-- LIST OF CONNECTED DRIVES ON THE SYSTEM --"
				lsblk
				read -p "Which drive corresponds to the SD card (/dev/sdX): " sdcard
				case $sdcard in
					/dev/sd* )
						test ! -b $sdcard && echo "Drive does not exist!" && exit;;
					* )
						echo "Wrong drive name!" && exit;;
				esac

				# create (if nonexistent) mountpoint
				test ! -d $rootfs && mkdir $rootfs

				# mount beaglebone root partition
				mountpoint -q $rootfs
				if [ $? -eq 0 ]; then
					echo "$rootfs already mounted!"
					exit
				else
					mount ${sdcard}2 $rootfs
				fi

				# Save current working directory
				wd=`pwd`

				echo "copying linux files..."
				cp='rsync -a --progress'

				cd ${wd}/kernel
				$cp arch/arm/boot/zImage ${rootfs}/boot
				$cp arch/arm/boot/dts/am335x-bone-common.dtsi ${rootfs}/boot/dtbs
				$cp arch/arm/boot/dts/am335x-bone.dts ${rootfs}/boot/dtbs
				$cp arch/arm/boot/dts/am335x-bone.dtb ${rootfs}/boot/dtbs
				$cp -r rootfs/lib ${rootfs}

				# set permissions
				chown -R root:root ${rootfs}/boot

				# umount root partition
				umount $rootfs && rm -rf $rootfs

				echo "Done! Extract the SD card"
			fi
			;;

		-h|--help|help)
			echo "usage: `basename $0` command"
			echo "available commands:"
			echo "  build"
			echo "  rebuild"
			echo "  clean"
			echo "  format"
			echo "  install"
			echo "  update"
			echo "  help"
			;;

		*)
			echo "usage: `basename $0` command"
			exit 1
	esac
fi
