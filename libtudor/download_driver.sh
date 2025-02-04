#!/bin/bash -e

HASH_FILE="$1"
TMP_DIR="$2"
OUT_DIR="$3"
DLLS=${@:4}

mkdir -p "$TMP_DIR"

#Download the driver executable and check hash
INSTALLER="$TMP_DIR/installer.cab"
wget https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2017/11/d2947925-9f18-44eb-8780-da632368234e_7abaa89fcc4dcfb7fc09a0a2c86de81660eeab36.cab -O "$INSTALLER"
shasum "$INSTALLER" | cut -d" " -f1 | cmp - "$HASH_FILE"

#Extract the driver
WINDRV="$TMP_DIR/windrv"
mkdir -p "$WINDRV"
cabextract -d "$WINDRV" "$INSTALLER"

#Copy outputs
mkdir -p "$OUT_DIR"
for dll in $DLLS
do
    cp $(find "$WINDRV" -name "$dll") "$OUT_DIR/$dll"
done
