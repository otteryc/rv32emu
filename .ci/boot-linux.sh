#!/usr/bin/env bash

function cleanup {
    sleep 1
    pkill -9 rv32emu
}

function ASSERT {
    $*
    local RES=$?
    if [ $RES -ne 0 ]; then
        echo 'Assert failed: "' $* '"'
        exit $RES
    fi
}

cleanup

ENABLE_VBLK=1
type dd >/dev/null 2>&1 || ENABLE_VBLK=0
(type mkfs.ext4 >/dev/null 2>&1 || type $(brew --prefix e2fsprogs)/sbin/mkfs.ext4) >/dev/null 2>&1 || ENABLE_VBLK=0
type 7z >/dev/null 2>&1 || ENABLE_VBLK=0

TIMEOUT=50
OPTS=" -k build/linux-image/Image "
OPTS+=" -i build/linux-image/rootfs.cpio "
OPTS+=" -b build/minimal.dtb "
if [ "$ENABLE_VBLK" -eq "1" ]; then
    dd if=/dev/zero of=build/disk.img bs=4M count=32
    mkfs.ext4 build/disk.img || $(brew --prefix e2fsprogs)/sbin/mkfs.ext4 build/disk.img
    OPTS+=" -x vblk:build/disk.img "
else
    printf "Virtio-blk Test...Passed\n"
fi
RUN_LINUX="build/rv32emu ${OPTS}"

if [ "$ENABLE_VBLK" -eq "1" ]; then
ASSERT expect <<DONE
set timeout ${TIMEOUT}
spawn ${RUN_LINUX}
expect "buildroot login:" { send "root\n" } timeout { exit 1 }
expect "# " { send "uname -a\n" } timeout { exit 2 }
expect "riscv32 GNU/Linux" { send "mkdir mnt && mount /dev/vda mnt\n" } timeout { exit 3 }
expect "# " { send "echo rv32emu > mnt/emu.txt\n" } timeout { exit 3 }
expect "# " { send "sync\n" } timeout { exit 3 }
expect "# " { send "umount mnt\n" } timeout { exit 3 }
expect "# " { send "\x01"; send "x" } timeout { exit 3 }
DONE
else
ASSERT expect <<DONE
set timeout ${TIMEOUT}
spawn ${RUN_LINUX}
expect "buildroot login:" { send "root\n" } timeout { exit 1 }
expect "# " { send "uname -a\n" } timeout { exit 2 }
expect "riscv32 GNU/Linux" { send "\x01"; send "x" } timeout { exit 3 }
DONE
fi
ret=$?
cleanup

MESSAGES=("OK!" \
     "Fail to boot" \
     "Fail to login" \
     "Fail to run commands" \
     "Fail to found emu.txt in disk.img"\
)

COLOR_G='\e[32;01m' # Green
COLOR_N='\e[0m' # No color
printf "\nBoot Linux Test: [ ${COLOR_G}${MESSAGES[$ret]}${COLOR_N} ]\n"
if [ "$ENABLE_VBLK" -eq "1" ]; then 
    file_list=`7z l build/disk.img`
    (echo $file_list | grep emu.txt) || ret=4
    printf "Virtio-blk Test: [ ${COLOR_G}${MESSAGES[$ret]}${COLOR_N} ]\n"
fi

exit ${ret}
