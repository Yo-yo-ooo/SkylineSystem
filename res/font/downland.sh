#!/usr/bin/env bash

# If this command runs failed you can try 
# wget https://github.com/user-attachments/files/29399496/consola.zip

FILE_URL="https://github.com/user-attachments/files/29399496/consola.zip"
ZIP_NAME="consola.zip"
TTF_NAME="consola.ttf"

if [ -f "$TTF_NAME" ]; then
    echo "$TTF_NAME already exists in this directory."
    exit 1
fi

# 1. 检查下载工具
if command -v curl >/dev/null 2>&1; then
    DOWNLOADER="curl -fsSLo"
elif command -v wget >/dev/null 2>&1; then
    DOWNLOADER="wget -qO"
else
    echo "Error: Neither curl nor wget found."
    exit 1
fi

# 2. 下载文件
echo "Downloading $ZIP_NAME..."
$DOWNLOADER "$ZIP_NAME" "$FILE_URL"
if [ $? -ne 0 ]; then
    echo "Error: Failed to download $ZIP_NAME"
    exit 1
fi

# 3. 检查 unzip
if ! command -v unzip >/dev/null 2>&1; then
    echo "Error: unzip not found, please install it first."
    rm -f "$ZIP_NAME"
    exit 1
fi

# 4. 解压并处理
echo "Extracting..."
if unzip -o "$ZIP_NAME"; then
    echo "Successfully extracted."
    rm -f "$ZIP_NAME"
else
    echo "Error: Failed to unzip $ZIP_NAME"
    exit 1
fi