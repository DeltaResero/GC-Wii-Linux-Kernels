***
**_Linux kernel for GameCube/Wii/vWii_**
***

This is still a work in progress but everything uploaded so far can now be seen in the various branches in this repository...  
Eventually this repository will become stable once I have the branches uploaded and fixed and this message will be removed...  

This is the GC/Wii Linux kernel branch.  Single branches of this repository can be downloaded by first selecting and following the instructions in the README.md in that branch.  A 
full copy of this repository can be downloaded by using git to clone the full repository as shown below.

    git clone https://github.com/DeltaResero/GC-Wii-Linux-Kernels.git

<br>

Compiling this kernel will has some dependencies that must be installed.  On a Debian based system, these dependecies can be installed by running the following command:

    sudo apt-get install advancecomp autoconfig automake bash build-essential bzip2 ccache coreutils fakeroot file gcc g++ gzip libmpfr-dev libgmp-dev libnurses5-dev make strip

<br>
***
**_About the Kernel_**  
***

PLACEHOLD FOR FUTURE INFORMATION

***
**_Known Bugs_**
***

SEE THE BUG SECTION OF EACH BRANCH FOR MORE INFO
<br>

***
**_General Information:_**  
***

- Both IOS and MINI also still suffer from the same hardware limitations that they did in 2.6.32.z.  For example, wireless and disc support for Wii consoles is still limited to MINI mode.  Also, DVDs can be mounted as they were in version 2.6.32.z, but due to hardware limitations, it's unable to write to any disc and is unable to read CDs and certain types of DVD's
    - Support for DVD-RW and DVD-DL disc seems to vary.  Currently, -R and +R (both mini & full-size) DVDs are know to work on both GameCube and Wii consoles.  All WiiU as well as some of the newer Wii disc drives, lack support for DVDs as they don't contain the same type of disc drive.  In other words, support will vary on the age of the console, but most standard GameCube consoles should be able to read mini DVDs (full-sized DVDs are too big for unmodified Gamecube consoles, but they can be read).
    - To mount a disc in a GameCube/Wii Linux distribution, try doing the following:

<br>

Create a "dvd" folder (as root) in the "/media" directory (only if the folder doesn't exist) with the command:

        mkdir /media/dvd

Then run the following (also as root):

        mount /dev/rvl-di /media/dvd

DVDs can be inserted/switched anytime but should be unmount prior to ejecting and then remount again after to prevent errors.  To unmount a disc, enter the following command as root:

        umount /dev/rvl-di /media/dvd

Note: Playing DVDs using a video player such as Mplayer or Xine will likely require that the disc be unmounted.  Instead of playing using the mount point, configure them to use the device "/dev/rvl-di" directly.
Additional packages such as libdvdcss & libdvdread may need to be installed for DVD playblack (may need to search package manager as naming standards aren't consistant).  Mplayer and Xine seem to work the best but support will vary depending on the operating system.  
<br>

_(Cross) Compiling the Kernel:_  

- Remember to edit the corresponding dts file (arch/powerpc/boot/)when not using the default boot method.  Also, enabling zcache/zswap require editing the dts bootargs.  See this for more info: https://bugs.archlinux.org/task/27267 & http://lwn.net/Articles/552791/

<br>

- A basic shell script was created to help in building the Kernel and should work on i386, X86_64, and PowerPC platforms.  To run this script, open a terminal and run (either "sh " or "./" followed by the name of the script):



        ./build-gc-wii-kernel.sh

    A basic menu should show if this script starts successfully.  The script & kernel both have a few dependencies (such as GNU Make) that are briefly mentioned near the top of this readme.
<br>

- For other basic (cross) compiling methods, see the following Wiki webpage at:  
http://www.gc-linux.org/wiki/Building_a_GameCube_Linux_Kernel_%28ARCH%3Dpowerpc%29
<br>

**_Compiled Filesystem/Kernel Demos_**  
	Wii IOS/MINI kernels:  WILL LINK ROOT FOLDER HERE WHEN IT BECOMES STABLE...  
	Debian:  https://spideroak.com/browse/share/DeltaResero/wii/Linux/Filesytems/Demos/Debian/
<br>
	Ubuntu:  PLANNING...
<br>
	Mint:  PLANNING...
<br>
- NOTE: Debian Sid/Jessie is not currently recommended, it's recommended to use  (stable) Wheezy or (old stable) Squeeze.  

**_Fully customized (more complete) desktop distributions images:_**  
	Debian Wheezy: PENDING...... (still UTC clock bugs)
<br>
	Debian Jessie: NOT READY YET (waiting for stability)
<br>
  Mint: PLANNING... (won't be ready until after Debian Wheezy) release
<br>  
	Ubuntu (ICEWM): NOT READY YET... (13.10 is completely unstable [non-kernel related])  
<br>

- Once the disk image is download and extracted, it can be copied to devices (such as SD cards) in Linux with the command:

        sudo dd if=4gbWheezyWii.img of=/dev/sdx#
        
Where x is the drive mount point letter (typically "b" or "c") and # is the partition number (usually "2").  **Be careful**, using the wrong sdx# device **may result in lost data**.
  (Replace "4gbWheezyWii.img" with name of the actual disc image if not using this one)  


The above links and Debian disk images use the following credentials:  
Username: wii  
Password: delta  
*Root password is the same (delta)  

For those who are looking to create their own Debian setup and perfer an interactive GUI, take a look at Farter's blog in the link below:

		http://fartersoft.com/blog/2011/08/17/debian-installer-for-wii/

For those who are having networking issues, take a look the links below for more help:

		http://www.linux-tips-and-tricks.de/overview#english
		http://www.linux-tips-and-tricks.de/downloads/collectnwdata-sh/download
		http://www.gc-linux.org/wiki/WL:Wifi_Configuration
		http://forum.wiibrew.org/read.php?29,68339,68339
<br>

_Notice(s):_  
- Reportedly most (if not all) GC/Wii Linux kernels (partially) work on WiiU in virtual Wii mode with proper setups.  Virtual Wii mode lacks support for non Wii hardware, Bluetooth, wireless, disc drive.  As with Wii consoles, currently USB support is currently broken.  For more information, see: http://gbatemp.net/threads/vwii-tri-core-linux.351024 and http://code.google.com/p/gbadev/
