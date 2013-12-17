***
**_Linux kernel for GameCube/Wii/vWii (Branch Version: 2.6.33.20 - Proof Of Concept)_**
***

This is the 2.6.33.y GC/Wii Linux kernel branch.  A full copy of this repository can be downloaded by using git to clone the full repository as shown below.

    git clone https://github.com/DeltaResero/GC-Wii-Linux-Kernels.git

To clone only a standalone copy of this branch, either download directly from Github from within the matching branch or releases section, or clone the branch with the following command (requires Git version 1.8.X or greater):

    git clone -b GC-Wii-Linux-Kernel-2.6.33.y --single-branch https://github.com/DeltaResero/GC-Wii-Linux-Kernels.git
<br>
For those who are using a version of Git prior to 1.8.X on Debian/Ubuntu based operating systems should able to easily update to at least an 1.8.X version via ppa.  The alternative is to compile Git from source (https://github.com/git/git).  By default any Linux operating system prior to Debian 7 and Ubuntu 13.10 use a version of Git that will require updating.  For Debian based systems, use the following commands to update Git (assuming git is already installed):

    sudo add-apt-repository ppa:pdoes/ppa
    sudo apt-get update
    sudo apt-get install git

Check which version of git is installed with the command: "git --version".  
<br>

Compiling this kernel will has some dependencies that must be installed.  On a Debian based system, these dependencies can be installed by running the following command:

    sudo apt-get install advancecomp autoconfig automake bash build-essential bzip2 ccache  coreutils fakeroot file gcc g++ gzip libmpfr-dev libgmp-dev libnurses5-dev make strip

<br>
***
**_About the Kernel_**  
***

The original (2.6.32 and prior) gcLinux work can be found at: http://sourceforge.net/projects/gc-linux/
This version of the GC/Wii Linux kernel, as with all that follow the official 2.6.32.y version, are forward ports of multiple patches that include (but are not limited to) the following:  

1. A modified MINI Kernel Preview 5 patch (http://www.gc-linux.org/wiki/MINI:KernelPreviewFive) with most mainline improvements integrated.
    - To clarify, I didn't write any of the original code, I just updated and merged existing code from various sources.  The existing code was altered so it would build against the newer 3.x based kernels.  I dubbed the changed patch as "MINI Kernel Preview 7".  I decided to reserve version 6 for the work that was done in the "origin/cocktail/gc-linux-2.6.34-rc5" branch of the gcLinux Git repository (git://git.infradead.org/users/herraa1/gc-linux-2.6.git).<br>  
2. Farter's Deferred I/O Framebuffer patch (http://fartersoft.com/)  
    - Apply this diff to get correct color output if not using the Xorg Cube driver.<br>  
3.  Additional default (defconfig) configurations (located in: "arch/powerpc/configs/")  
    - (Currently: "gamecube_smaller", "wii-ios-mode", and "wii-mini-mode")<br>  


<br>

***
**_Known Bugs_**
***

Due to significant changes since the last official kernel patch release gcLinux, the 2.6.32 (MIKEp5) patch, the MIKEp7 patch currently has some limitations and bugs that still require attention.  Some of these limitations are as follows:  

- Both IOS and MINI modes seem to have a bug that prevents Linux from booting if a GameCube Controller is inserted in one of the ports while the serial port is enabled in the config.  This bug is caused by a glitch that was created when forward porting from 2.6.32 to 2.6.33.  It should be possible to find this bug using git bisect.

- Only Cube Xorg or Farter's Framebuffer can be used, not both at the same time.  If Xorg is setup to use Cube on the target system, do not use a kernel that was compiled with Farter's framebuffer patch without adjusting the Xorg configuration file (usually in: /etc/X11/xorg.conf).  Using both simultaneously will cause the display to show nothing at best.  Due to this, I've made Farter's framebuffer patch optional by leaving a copy of it separate in the source so that it could be patched manually at anytime.
<br>

    To patch, enter the command:

        "patch < 0001-vfb-defio-wii.diff -p1"

    To remove the patch, enter the command:

        "patch < 0001-vfb-defio-wii.diff -p1 -R"  

- Both IOS and MINI also still suffer from the same hardware limitations that they did in 2.6.32.y.  For example, wireless and disc support for Wii consoles is still limited to MINI mode.  Also, DVDs can be mounted as they were in version 2.6.32.z, but due to hardware limitations, it's unable to write to any disc and is unable to read CDs and certain types of DVD's
    - Support for DVD-RW and DVD-DL disc seems to vary.  Currently, -R and +R (both mini & full-size) DVDs are know to work on both GameCube and Wii consoles.  All WiiU as well as some of the newer Wii disc drives, lack support for DVDs as they don't contain the same type of disc drive.  In other words, support will vary on the age of the console, but most standard GameCube consoles should be able to read mini DVDs (full-sized DVDs are too big for unmodified GameCube consoles, but they can be read).
    - To mount a disc in a GameCube/Wii Linux distribution, try doing the following:

<br>

Create a "dvd" folder (as root) in the "/media" directory (only if the folder doesn't exist) with the command:

        mkdir /media/dvd

Then run the following (also as root):

        mount /dev/rvl-di /media/dvd

DVDs can be inserted/switched anytime but should be unmount prior to ejecting and then remount again after to prevent errors.  To unmount a disc, enter the following command as root:

        umount /dev/rvl-di /media/dvd

Additional packages such as libdvdcss & libdvdread may need to be installed for DVD playback (may need to search package manager as naming standards aren't consistent).  Mplayer and Xine seem to work the best but support will vary depending on the operating system.  
<br>

***
**_General Notices:_**  
***

_(Cross) Compiling the Kernel:_  

- Remember to edit the corresponding dts file (arch/powerpc/boot/)when not using the default boot method.  Also, enabling Zcache requires editing the dts dts bootargs.  See this for more info: https://bugs.archlinux.org/task/27267

<br>

- A basic shell script was created to help in building the Kernel and should work on i386, X86_64, and PowerPC platforms.  To run this script, open a terminal and run (either "sh " or "./" followed by the name of the script):



        ./build-gc-wii-kernel.sh

    A basic menu should show if this script starts successfully.  The script & kernel both have a few dependencies (such as GNU Make) that are briefly mentioned near the top of this readme.
<br>

- For other basic (cross) compiling methods, see the following Wiki web page at:  
http://www.gc-linux.org/wiki/Building_a_GameCube_Linux_Kernel_%28ARCH%3Dpowerpc%29
<br>


For those who are looking to create their own Debian setup and prefer an interactive GUI, take a look at Farter's blog in the link below:

		http://fartersoft.com/blog/2011/08/17/debian-installer-for-wii/

For those who are having networking issues, take a look the links below for more help:

		http://www.linux-tips-and-tricks.de/overview#english
		http://www.linux-tips-and-tricks.de/downloads/collectnwdata-sh/download
		http://www.gc-linux.org/wiki/WL:Wifi_Configuration
		http://forum.wiibrew.org/read.php?29,68339,68339
<br>

_Notice(s):_  
- Reportedly most (if not all) GC/Wii Linux kernels (partially) work on WiiU in virtual Wii mode with proper setups.  Virtual Wii mode lacks support for non Wii hardware, Bluetooth, wireless, disc drive.  As with Wii consoles, currently USB support is currently broken.  For more information, see: http://gbatemp.net/threads/vwii-tri-core-linux.351024 and http://code.google.com/p/gbadev/
