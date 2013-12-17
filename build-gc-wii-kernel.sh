#!/bin/bash
#
# Linux Shell Script For Compiling Wii Linux Kernels
#
# Written by DeltaResero <deltaresero@zoho.com>
#


#Set defaults and checks if arch is supported by this script 
clear
echo "This basic script is meant as a way to help with cross compiling"
echo "GameCube and Wii Kernels.  While this isn't very robust, it should"
echo -e "suffice for most basic compilations.\n"

echo "Compiling a kernel usually requires the following dependencies at minimal:"
echo "advancecomp (advdef), autoconfig, automake, bash, build-essential, busybox,"
echo "bzip2, ccache, coreutils, fakeroot, file, gcc, g++, gizp, libmpfr-dev,"
echo "libgmp-dev, libnurses5-dev, strip."
echo -e "While there are other dependencies, these are the most common ones."
echo "If any are missing, it's highly recommended that this script be stopped"
echo -e "and these dependency packages be installed before continuing.\n"

buildTarget=''
useConfig=''
MACHINE_TYPE=`uname -m`

if [[ ${MACHINE_TYPE} != 'x86_32'&& ${MACHINE_TYPE} != 'x86_64' && ${MACHINE_TYPE} != 'ppc' ]]; then
  echo "ARCH: Unsupported -" $MACHINE_TYPE
  echo "Quitting script..."
  exit 0
fi

#Gets the user to select a console target (GameCube or Wii)
while :
do
  echo "Enter a value below matching the target platform or enter 4 to use '.config':"
  echo "(Selecting 'quit' at any time quit will exit this script)"
  echo "1) GameCube"
  echo "2) Wii"
  echo "3) Other/Use an existing .config"
  echo "4) Quit Script"
  echo -n "Response: "
  read opt
  case $opt in
    1) echo "Selecting target: GameCube"
       buildTarget='GameCube'
       break;;
    2) echo "Selecting target: Wii"
       buildTarget='Wii'
       break;;
    3) echo "Selecting target: Unknown (Not needed if .config has already been created)"
       buildTarget='Unknown'
       break;;
    4) echo "Quitting script..."
       exit 0;;
    *) echo -e "\n$opt is an invalid option."
       echo "Please select an option from 1-4 only"
       echo "Press [enter] key to continue. . ."
       read enterKey
       clear;;
  esac
done

#Clear Screen
clear

if [ ${buildTarget} != 'Unknown' ]; then
  #Attempts to get user to select a base configuration to start with
  while :
  do
    echo "Enter a numerical value corresponding to the configuration to be used"
    echo "(Remember to edit the platform dts 'bootags' to correspond to the build type)"
    echo "1) Isobel's  - Default / With Modules (Wii MINI, GameCube - Default)"
    echo "2) DeltaResero's - Minimalist / No Modules (Wii MINI, GameCube - Small)"
    echo "3) DeltaResero's - Minimalist / No Modules (Wii IOS, GameCube - Same as 2 for now)"
    echo "4) Quit Script"
    echo -n "Response: "
    read opt2
    case $opt2 in
      1) if [ ${buildTarget} = 'GameCube' ]; then
          echo "Selecting config: gamecube_defconfig"
          useConfig='gamecube_defconfig'
        elif [ ${buildTarget} = 'Wii' ]; then
          echo "Selecting config: wii_defconfig"
          useConfig='wii_defconfig'
        fi
        break;;
      2) if [ ${buildTarget} = 'GameCube' ]; then
          echo "Selecting config: gamecube_smaller_defconfig"
          useConfig='gamecube_smaller_defconfig'
        elif [ ${buildTarget} = 'Wii' ]; then
          echo "Selecting config: wii-mini-mode_defconfig"
          useConfig='wii-mini-mode_defconfig'
        fi
        break;;
      3) if [ ${buildTarget} = 'GameCube' ]; then
          echo "Selecting config: gamecube_smaller_defconfig"
          useConfig='gamecube_smaller_defconfig'
        elif [ ${buildTarget} = 'Wii' ]; then
          echo "Selecting config: wii-ios-mode_defconfig"
          useConfig='wii-ios-mode_defconfig'
        fi
        break;;
      4) echo "Quitting script..."
        exit 0;;
      *) echo -e "\n$opt is an invalid option."
        echo "Please select option from 1-4 only"
        echo "Press [enter] key to continue. . ."
        read enterKey
        clear;;
    esac
  done

else
  echo "Selecting config: .config"
  useConfig='.config'
fi


#Find number of processors for setting number of parallel jobs
numProcessors=$(grep -c ^processor /proc/cpuinfo)
echo "Detected number of processors:" ${numProcessors}

if [ ${MACHINE_TYPE} = 'x86_32' ]; then
  MACHINE_TYPE='Intel 80386'
  echo "ARCH: 32-bit -" ${MACHINE_TYPE}
  export LD_LIBRARY_PATH=H-i686-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/lib

  if [ ${useConfig} != '.config' ]; then
    make ${useConfig} ARCH=powerpc CROSS_COMPILE=H-i686-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
  fi

  make menuconfig ARCH=powerpc CROSS_COMPILE=H-i686-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
  make clean ARCH=powerpc CROSS_COMPILE=H-i686-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
  make -j${numProcessors} ARCH=powerpc CROSS_COMPILE=H-i686-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-


elif [ ${MACHINE_TYPE} = 'x86_64' ]; then
  MACHINE_TYPE='x86-64'
  echo "ARCH: 64-bit -" ${MACHINE_TYPE}
  export LD_LIBRARY_PATH=H-x86_64-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/lib

  if [ ${useConfig} != '.config' ]; then
    make ${useConfig} ARCH=powerpc CROSS_COMPILE=H-x86_64-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
  fi

  make menuconfig ARCH=powerpc CROSS_COMPILE=H-i686-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
  make clean ARCH=powerpc CROSS_COMPILE=H-x86_64-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
  make -j${numProcessors} ARCH=powerpc CROSS_COMPILE=H-x86_64-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-


else # Arch must be 32 bit powerpc as it's the only choice left
  MACHINE_TYPE='PowerPC'
  echo "ARCH: 32-bit -" ${MACHINE_TYPE}

  if [ ${useConfig} != '.config' ]; then
    make ${useConfig}
  fi

  make clean
  make menuconfig
  make -j${numProcessors}
fi

#Sets modules and temp folder paths
TEMP_DIRECTORY='../modules'
MOD_DIRECTORY='modules'

#Checks if the temporary directory exist (quits if it does already)
if [ -d "$TEMP_DIRECTORY" ]; then
  echo "Error: Temp directory already exist at -" $TEMP_DIRECTORY
  echo "Script quitting..."
  echo "Manually run: 'find -name '*.ko' -exec cp -av {} ../modules \;'"
  echo "to copy files to modules folder a level up.  You may have to"
  echo "create a modules folder with 'mkdir ../modules' prior..."
  exit
fi

#Creates the temporary directory
echo "Creating temp folder:" $TEMP_DIRECTORY
mkdir $TEMP_DIRECTORY

#Removes modules directory if it already exist
if [ -d "$MOD_DIRECTORY" ]; then
  echo "Removing existing modules folder in current directory..."
  rm -R $MOD_DIRECTORY
fi

#Checks for modules and places them in a temp folder and then moves them to the modules folder
find -name '*.ko' -exec cp -av {} $TEMP_DIRECTORY \;
echo -e "Moving temp modules folder ("$TEMP_DIRECTORY") into current directory...\n"
mv $TEMP_DIRECTORY .

while :
do
  echo "Reduce kernel size by running super strip?"
  echo "1) Yes"
  echo "2) No"
  echo -n "Response: "
  read opt3
  case $opt3 in
    1) echo "Super strip selected..."
        echo -e "Checking for installed Super Strip (sstrip)..."
        #Strip zImage even farther (if possible)
        strip=./sstrip
        zImageFile=arch/powerpc/boot/zImage
        zImageInitrdFile=arch/powerpc/boot/zImage.initrd
        #Locate sstrip
        if [ sstrip -v]; then #Use installed sstrip
          echo "sstrip found..."
          #Checks for zImage and runs sstrip on it
          if [ -f $zImageFile ]; then
            echo "Stripping zImage..."
            ./sstrip -z $zImageFile
          #Checks for zImage.initrd and runs sstrip on it
          elif [ -f $zImageInitrdFile ]; then
            echo "Stripping zImage.initrd"
            ./sstrip -z $zImageInitrdFile
          else #No zImage (broken build)
            echo "Error, zImage (Kernel) not found!"
            echo "Quitting script..."
            exit 1
          fi
        else #Use local sstrip
          echo "external sstrip not found, using local build"
          #Checks for an already existing local sstrip from a previous build
          if [ -f $strip ]; then
            echo "Found sstrip..."
          else
            echo "Building local sstrip..."
            make -C super-strip
            mv super-strip/sstrip sstrip
          fi

          #Checks if local sstrip is executable
          if [ -x "$strip" ]; then
            echo "The sstrip binary is executable..."
          else
            echo "Attempting to make sstrip executable..."
            chmod +x sstrip
          fi

          #Checks if built sstrip is same arch as host
          if [ $(file $strip | grep -ci ${MACHINE_TYPE}) == '1' ]; then
            echo "sstrip is correct arch (same as host machine)..."
          else
            echo "sstrip is not correct arch (different than host machine)..."
            echo "Removing sstrip..."
            rm sstrip

            echo "Building sstrip..."
            make -C super-strip
            mv super-strip/sstrip sstrip

            echo "Attempting to make sstrip executable..."
            chmod +x sstrip

            echo "Cleaning sstrip build files..."
            make clean -C super-strip
          fi

          #Checks for zImage and runs sstrip on it
          if [ -f $zImageFile ]; then
            echo "Stripping zImage..."
            ./sstrip -z $zImageFile
          #Checks for zImage.initrd and runs sstrip on it
          elif [ -f $zImageInitrdFile ]; then
            echo "Stripping zImage.initrd"
            ./sstrip -z $zImageInitrdFile
          else #No zImage (broken build)
            echo "Error, zImage (Kernel) not found!"
            echo "Quitting script..."
            exit 1
          fi
        fi #End of else condition for checking for sstrip
        echo -e "WARNING: DO NOT STRIP KERNEL ELF WITH STRIP, IT WAS STRIPPED WITH SSTRIP"
        echo "(Stripping the zImage with strip will result in corruption!)"
        break;;

    2) echo "Super strip (sstrip) not selected..."
        break;;

    *) echo -e "\n$opt is an invalid option."
       echo "Please select option from 1-4 only"
       echo "Press [enter] key to continue. . ."
       read enterKey
       clear;;
  esac
done

#Script is finished (everything should have been successful upon reaching here)
echo -e "\nDone! (Check to see if there were any errors above)\n"
echo "The binary (zImage) can be found in: 'arch/powerpc/boot'"
echo "Modules (if any) should be located in the folder:" $MOD_DIRECTORY
exit 0
#
#
# Requires a buildroot PowerPC cross compiler for x86 systems
# More info on this tool can be found at the following website:
# http://www.gc-linux.org/wiki/Building_a_GameCube_Linux_Kernel_%28ARCH%3Dpowerpc%29
#
