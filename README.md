***
# Linux kernel for GameCube/Wii
***

**_About Repository_**

This is a Repository containing the source of a port of the Linux operating system kernel for GameCube and Wii. After Mini Kernel Preview 5 was released for the 2.6.32 Linux kernel, efforts shifted towards getting what existed into mainline and the patches no longer were maintained by the community. As 2.6.32 was a long term kernel, this wasn't seen as an immediate issue and the kernel recieved updates and was support until it reached End Of Life status in February of 2016, and hence this repository was created in late 2013 efforts to attempt to port the patches over to the newer kernels and work alongside what got merged in mainline. This has been mostly sucessful in that most of the hardware continues to work with the newer 3.x kernels, but some work still remains to get everything working again as it once did. After reaching 3.12, this repository seen no updates from 2014 till late 2017 after reaching a problem with SDHC that stood in the way. Picking up where this left off, Neagix has was able to work around the issue and began working on other issues that stood in the way in getting the GC/Wii patches working with modern kernels. His work can be found at: https://github.com/neagix/wii-linux-ngx 

Due to differences in how Virtual Wii mode works on Wii U consoles, this will not work as is without some minor modifications. For Virtual Wii, see the following: http://gbatemp.net/threads/vwii-tri-core-linux.351024 https://gbatemp.net/threads/coding-vwii-3-core-support-everything-you-need-to-know.347626/ https://github.com/crowell/gbadev.kernel

<br>

**_About GC/Wii/vWii Linux_**

Back in December of 2003, a group from the Xbox Linux project and members of the GameCube Homebrew scene came together at the [Chaos Communication Congress](http://www.linuxdevcenter.com/pub/a/linux/2004/04/01/warp_pipe.html) in Berlin, Germany. In January of 2004, they released a Linux preview for the Nintendo GameCube. This preview would draw the Linux mascot, Tux, on the GameCube console giving a small preview of the group's ultimate goal of bring Linux to the GameCube.  While not that powerful by modern standards, the Nintendo GameCube could hold it's own at the time. Steadily till 2006, there were several developments adding support over time to nearly every part of the GameCube and it's peripherals.

For the next few years, development went mostly silent, but the kernel patches were kept up to date and support had been added for the USB Gecko adapter. By February of 2008, Linux was running natively on Wii consoles using Team Tweezers' twilight-hack and a small proof of concept demo was released. Work shifted over towards support the Wii console and continued on steadily till the end of 2009. At that point, the strive was more towards getting support into mainline kernels than towards new developments. Once the [Mini Kernel Preview 5 patch (MIKEp5)](http://www.gc-linux.org/wiki/MINI:KernelPreviewFive) was release for 2.6.32, work shifted from maintaining the patches to getting support in mainline. While some of the hardware got support it needed added in, a lot of it didn't and over time interest greatly decline after 2010 as gcLinux slowly became dormant. For a more complete view of what happened and for additional information not covered here, please visit their homepage at: http://www.gc-linux.org/wiki/Main_Page

For a short while after their last patch was released, a few projects were worked on that affected the hardware, but they didn't only added to the stuff in the MIKEp5 patch and didn't update what was already there. A few attempts were made to rebase the kernel patches by members in the community, but it wasn't till late 2013 that this was successful and even then it was a limited success as it broke compatibility with some hardware. During this time, a group from GBATemp got Virtual Wii mode booting into Linux after some patching. While it did boot, there were issue with compatibility and the extra RAM and CPU couldn't be utilized in it's state at the time. They tried and made a lot of progress, but there just wasn't enough interest and as of 2017 the project appears to be mostly at a standstill.

<br>

**_Known Issues_**

* Both IOS and MINI also still suffer from the same hardware limitations that they did in 2.6.32.y.  For example, wireless and disc support for Wii consoles is still limited to MINI mode.  Also, DVDs can be mounted as they were in version 2.6.32.y, but due to hardware limitations, it's unable to write to any disc and is unable to read CDs and certain types of DVD's
    - Support for DVD-RW and DVD-DL disc seems to vary.  Currently, -R and +R (both mini & full-size) DVDs are know to work on both GameCube and Wii consoles.
    All WiiU as well as some of the newer Wii disc drives, lack support for DVDs as they don't contain the same type of disc drive.
    In other words, support will vary on the age of the console, but most standard GameCube consoles should be able to read mini DVDs (full-sized DVDs are too big for unmodified GameCube consoles, but they can be read).
    

* Bugs probably introduced in the port of MIKEp5 from v2.6 to v3.x tree: *
    - In IOS mode, external swap partitions don't mount correctly as of kernel version 2.6.39. As a workaround, use a local swapfile (This bug should be relatively easy to find using git bisect)
    - Both IOS and MINI modes seem to have a bug that prevents Linux from booting if a GameCube Controller is inserted in one of the ports while the serial port is enabled in the config.  This bug is caused by a glitch that was created when forward porting from 2.6.32 to 2.6.33.  It should be possible to find this bug using git bisect.
    
See [open issues](https://github.com/DeltaResero/GC-Wii-Linux-Kernels/issues).

<br>

**_Retrieving the Sources_**

This is the GC/Wii Linux kernel master branch.  A full copy of this repository can be downloaded by using git to clone the full repository as shown below.

    git clone https://github.com/DeltaResero/GC-Wii-Linux-Kernels.git

To clone only single branch containing only one specific kernel, either download directly from Github from within the matching branch, or clone the branch with the following command (requires Git version 1.8.X or greater):

    git clone -b GC-Wii-Linux-Kernel-3.12.y --single-branch https://github.com/DeltaResero/GC-Wii-Linux-Kernels.git


For those using a version of Git prior to 1.8.X, the above command will not work. If the distribution doesn't have a version of at least 1.8.x, you can compile Git from source (https://github.com/git/git). Alternatively, using a PPA such as [ppa:pdoes/ppa](https://launchpad.net/~pdoes/+archive/ubuntu/ppa) for an Ubuntu based system may be another option too. To check which version of git is installed, use the command: "git --version".  

<br>


**_Getting Started with GameCube_**

With a 486 MHz G3 PowerPC processor, 40MB of RAM (24MB of main RAM with 16MB of VRAM), 3MB for embedded GPU, a DVD-ROM drive allowing for miniDVDs or DVDs via a case mod/removal, and an optional Ethernet adapter, the GameCube is capable of running a basic operating system. While the patched kernel sources here can be used to build kernels for the GameCube, the focus here will mostly be on Wii. For more information on how to set up Homebrew on a GameCube. see here: https://sdremix.com/installation/gamecube-installation/running-gcn-homebrew/

** _Getting Started with Wii_**

With a 729MHz G3 PowerPC processor, 88MB of RAM (64MB GDDR3 with 24MB 1T-SRAM), 3MB for embedded GPU, an optical disc drive, an optional USB Ethernet adapter, SD/SDHC card reader, two USB 2.0 ports, Wi-Fi IEEE 802.11 b/g, and Bluetooth, the Wii is very capable of running a lightweight operating system. To install Linux on the Wii and use this kernel, Homebrew should be set up first. There are many ways of doing this and if this hasn't been done yet, it is recommended to follow the guide over at http://www.wiibrew.org/wiki/Homebrew_setup

Although Linux may be loaded through the Homebrew Channel on Wii, it is recommended to be loaded through bootmii/MINI to access all the RAM, enable Wi-Fi, Bluetooth, USB2.0 support, and to be able to use the optical drive. While the kernel itself can communicate with nearly all Wii peripherals; Wii Remotes, Wi-Fi, USB, Bluetooth, GameCube controllers, USBGeckos,  and the DVD drive, GameCube Memory Cards are currently unsupported and do not work currently in kernels past the 2.6.32.y. To select between IOS and Mini, change loader=mini or loader=ios in the bootargs of "arch/powerpc/boot/dts/wii.dts" and also set the location of root here.

<br>

**_Compiling the Kernel Sources_**

Compiling this kernel will has some dependencies that must be installed.  On a Debian based system, these dependencies can be installed by running the following command:

    sudo apt-get install advancecomp autoconfig automake bash build-essential bzip2 ccache coreutils fakeroot file gcc g++ gzip libmpfr-dev libgmp-dev libnurses5-dev make strip

To cross compile with a modern cross compiler (recommended), you will also need a PowerPC toolchain

    sudo apt-get install gcc-powerpc-linux-gnu


- Remember to edit the corresponding dts file (arch/powerpc/boot/dts).  Also, enabling zcache/zswap require editing the dts bootargs.  See this for more info: https://bugs.archlinux.org/task/27267 & http://lwn.net/Articles/552791/

- A basic shell script was created and left in the master branch to help in building the kernel. While it won't cover compiling in every situation it should work on i386, X86_64, and PowerPC platforms for most basic compilations. To use this script to help with cross compiling the kernel source, copy this script to inside the kernel source folder and open a terminal and run (either "sh " or "./" followed by the name of the script):

        ./build-gc-wii-kernel.sh

    A basic menu should show if this script starts successfully. If you would like to use the 2009 PowerPC toolchain included here in the master branch, copy the H-i686-pc-linux-gnu and H-x86_64-pc-linux-gnu over along with the build-gc-wii-kernel.sh script to inside the kernel source folder (where this README.md is located).  


- For other basic (cross) compiling methods, see the following Wiki webpage at:  
http://www.gc-linux.org/wiki/Building_a_GameCube_Linux_Kernel_%28ARCH%3Dpowerpc%29


<br>

***
**_General Information:_**  
***

To mount a disc in a GameCube/Wii Linux distribution, try doing the following:

- Create a "dvd" folder (as root) in the "/media" directory (only if the folder doesn't exist) with the command:

        mkdir /media/dvd

- Then run the following (also as root):

        mount /dev/rvl-di /media/dvd

- DVDs can be inserted/switched anytime but should be unmount prior to ejecting and then remount again after to prevent errors.  To unmount a disc, enter the following command as root:

        umount /dev/rvl-di /media/dvd

Note: Playing DVDs using a video player such as Mplayer or Xine will likely require that the disc be unmounted.  Instead of playing using the mount point, configure them to use the device "/dev/rvl-di" directly.
Additional packages such as libdvdcss & libdvdread may need to be installed for DVD playblack (may need to search package manager as naming standards aren't consistant).  Mplayer and Xine seem to work the best but support will vary depending on the operating system.  

<br>

For those who are looking to create their own Debian setup and prefer an interactive GUI, take a look at Farter's blog in the link below:

		http://fartersoft.com/blog/2011/08/17/debian-installer-for-wii/

For those who are having networking issues, take a look the links below for more help:

		http://www.linux-tips-and-tricks.de/overview#english
		http://www.linux-tips-and-tricks.de/downloads/collectnwdata-sh/download
		http://www.gc-linux.org/wiki/WL:Wifi_Configuration
		http://forum.wiibrew.org/read.php?29,68339,68339
