#!/bin/bash

READER_BIN="$(dirname "$0")/../bin/xrcomm_nvme_reader"

# Ensure the script is run with sudo privileges
if [ "$EUID" -ne 0 ]; then
  echo -e "\e[31m[ERROR]\e[0m Please run as root: sudo ./extract_nvme.sh"
  exit 1
fi

if [ ! -f "$READER_BIN" ]; then
    echo -e "\e[31m[ERROR]\e[0m Cannot find $READER_BIN. Installation may be corrupt."
    exit 1
fi

clear
echo -e "\e[36m╔═══════════════════════════════════════════════════════╗\e[0m"
echo -e "\e[36m║         XRComm NVMe Extraction Dashboard v1.0.1         ║\e[0m"
echo -e "\e[36m╚═══════════════════════════════════════════════════════╝\e[0m"
echo ""

# 1. Select Extraction Mode
echo -e "\e[33mSelect Extraction Mode:\e[0m"
echo "  1) Extract ALL Data (Full Event)"
echo "  2) Extract FIRST portion (Start of run)"
echo "  3) Extract MIDDLE portion (Steady-state)"
echo "  4) Extract LAST portion (End of run)"
read -p "Enter choice [1-4]: " MODE_CHOICE

CMD_BASE="$READER_BIN"
SIZE_FLAG=""

if [ "$MODE_CHOICE" != "1" ]; then
    echo ""
    echo -e "\e[33mSelect Size Unit:\e[0m"
    echo "  1) Megabytes (MB)"
    echo "  2) Gigabytes (GB)"
    echo "  3) Raw Blocks/Records"
    read -p "Enter choice [1-3]: " UNIT_CHOICE

    read -p "Enter the amount (e.g., 50, 1.5, 1000): " AMOUNT

    case $MODE_CHOICE in
        2) PREFIX="--first" ;;
        3) PREFIX="--middle" ;;
        4) PREFIX="--last" ;;
        *) echo "Invalid mode."; exit 1 ;;
    esac

    case $UNIT_CHOICE in
        1) SIZE_FLAG="${PREFIX}-mb=${AMOUNT}" ;;
        2) SIZE_FLAG="${PREFIX}-gb=${AMOUNT}" ;;
        3) SIZE_FLAG="${PREFIX}-records=${AMOUNT}" ;;
        *) echo "Invalid unit."; exit 1 ;;
    esac
fi

# 2. Select Flash (Deletion) Options
echo ""
echo -e "\e[33mPost-Extraction Action:\e[0m"
echo "  1) Keep data on drive (Safe)"
echo "  2) Delete event ONLY if extraction is 100% successful (--flash)"
echo "  3) FORCE delete event even if partial/corrupted (--force-flash)"
read -p "Enter choice [1-3]: " FLASH_CHOICE

FLASH_FLAG=""
case $FLASH_CHOICE in
    1) FLASH_FLAG="" ;;
    2) FLASH_FLAG="--flash" ;;
    3) FLASH_FLAG="--force-flash" ;;
    *) echo "Invalid choice. Defaulting to keep data."; FLASH_FLAG="" ;;
esac

# 3. Build and Execute Command
echo ""
echo -e "\e[32mExecuting Command:\e[0m"
FINAL_CMD="$CMD_BASE $SIZE_FLAG $FLASH_FLAG"

# Trim extra spaces
FINAL_CMD=$(echo "$FINAL_CMD" | xargs)
echo -e "\e[1;37m$FINAL_CMD\e[0m"
echo "---------------------------------------------------------"

eval "$FINAL_CMD"

echo "---------------------------------------------------------"
echo -e "\e[32m[DONE]\e[0m Extraction script finished."