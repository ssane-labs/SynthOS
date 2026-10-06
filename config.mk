# Build-time configuration. Turn features off to shrink the kernel for tiny machines.

CONFIG_DOS      ?= 1
CONFIG_SXVM     ?= 1
CONFIG_EDITOR   ?= 1
IMAGE_SECTORS   ?= 8192

CONFIG_DEFINES := -DCONFIG_DOS=$(CONFIG_DOS) -DCONFIG_SXVM=$(CONFIG_SXVM) -DCONFIG_EDITOR=$(CONFIG_EDITOR)
