#!/system/bin/sh
# Run on the phone in NORMAL Android (slot a), as root:
#   adb push rosemary-facts.sh /data/local/tmp/ && adb shell su -c 'sh /data/local/tmp/rosemary-facts.sh' > facts.txt
echo "### chosen props"; ls /proc/device-tree/chosen/
echo "### bootargs"; cat /proc/device-tree/chosen/bootargs; echo
echo "### atag,videolfb (raw)"; od -An -tx1 /proc/device-tree/chosen/atag,videolfb
for p in fb_base_l fb_base_h vramSize islcmfound islcm_inited fps; do
  echo "### atag,videolfb-$p"; od -An -tx1 /proc/device-tree/chosen/atag,videolfb-$p 2>&1
done
echo "### lcmname"; cat /proc/device-tree/chosen/atag,videolfb-lcmname 2>&1; echo
echo "### memory reg"; for d in /proc/device-tree/memory*; do echo $d; od -An -tx1 $d/reg; done
echo "### reserved-memory"
for d in /proc/device-tree/reserved-memory/*/; do echo "$d"; od -An -tx1 "$d/reg" 2>/dev/null; done
echo "### iomem"; head -80 /proc/iomem
echo "### cpuinfo"; cat /proc/cpuinfo
echo "### dmesg fb/display/emmc/wdt"
dmesg | grep -i -E "fb_base|mtkfb|videolfb|lcm_name|vram|framebuffer|mmc0|msdc0|wdt|rgu|bootreason|Booting Linux|CPU features|page size|Kernel command" | head -60
echo "### props"; getprop | grep -i -E "ro.boot.(slot|bootreason|hardware|verifiedboot|vbmeta|veritymode)|ro.hardware|ro.product.(device|name)|ro.build.fingerprint" 
echo "### partitions"; ls -l /dev/block/by-name | head -80
