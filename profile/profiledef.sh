iso_name="binbows98"
iso_label="BINBOWS98"
iso_publisher="Michaelsoft Publishing"
iso_application="Binbows 98"
iso_version="1.0"
iso_arch="x86_64"
buildmodes=('iso')
bootmodes=('bios.syslinux.mbr' 'bios.syslinux.eltorito' 'uefi-ia32.grub.esp' 'uefi-x64.grub.esp')
airootfs_image_type="squashfs"
airootfs_image_tool_options=('-comp' 'xz' '-Xbcj' 'x86' '-b' '1M' '-Xdict-size' '1M')
file_permissions=(
  ["/etc/shadow"]="0:0:400"
  ["/usr/local/bin/binbows-start"]="0:0:755"
  ["/usr/local/bin/binbows-install"]="0:0:755"
  ["/usr/local/bin/binbows-installer"]="0:0:755"
  ["/opt/binbows/netstay/netstay"]="0:0:755"
  ["/opt/binbows/binamp/binamp"]="0:0:755"
)