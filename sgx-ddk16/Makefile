SHELL := /bin/sh

SDK_VERSION := 4.03.00.02
DDK_VERSION := 1.6.16.3977
BUILD_DIR ?= $(CURDIR)/build
DIST_DIR ?= $(CURDIR)/dist
SDK_INSTALLER ?=
SDK_URL ?=
SDK_SHA256 ?=
ACCEPT_TI_EULA ?= no
INSTALLER := $(BUILD_DIR)/downloads/Graphics_SDK_setuplinux_4_03_00_02.bin
SDK_ROOT := $(BUILD_DIR)/sdk
AUDIT_DIR := $(BUILD_DIR)/audit
HF_SHIM_DIR := $(BUILD_DIR)/hf-shim
TOOLS_DIR := $(CURDIR)/../tools
CROSS_HF ?= arm-linux-gnueabihf-gcc
PATCHELF ?= patchelf

# DDK 1.4 SDK (meta-openpandora Graphics SDK 4.00.00.01 -> DDK 1.4.14.2616).
# The stripped 1.4 libs reuse the 1.6-DWARF veneers; its headers drive the
# ABI header-diff gate against the 1.6 headers.
SDK14_VERSION := 4.00.00.01
DDK14_VERSION := 1.4.14.2616
SDK14_INSTALLER ?=
SDK14_URL ?=
SDK14_SHA256 ?=
INSTALLER14 := $(BUILD_DIR)/downloads/Graphics_SDK_setuplinux_4_00_00_01.bin
SDK14_ROOT := $(BUILD_DIR)/sdk14
HF_SHIM14_DIR := $(BUILD_DIR)/hf-shim-14

.DEFAULT_GOAL := help

.PHONY: help fetch unpack audit dev-tree hf-shim fetch14 unpack14 dev-tree14 hf-shim-14 package verify clean

help:
	@printf '%s\n' \
		'SGX DDK 1.6 userspace package pipeline' \
		'' \
		'  make fetch    Download or copy the checksum-pinned TI installer' \
		'  make unpack   Extract gfx_rel_es2.x after explicit EULA acceptance' \
		'  make audit    Record ELF attributes, dependencies, imports, and exports' \
		'  make dev-tree Stage public TI headers and pkg-config metadata' \
		'  make hf-shim  Build the hard-float ABI shim set (needs armhf cross gcc)' \
		'  make hf-shim-14 Build the DDK 1.4 shim by reusing the 1.6 veneers' \
		'                (needs SDK14_URL + SDK14_SHA256 for the 4.00.00.01 SDK)' \
		'  make package  Build the runtime, tools, and development packages' \
		'  make verify   Inspect package contents and run static ELF checks' \
		'' \
		'Required inputs:' \
		'  SDK_INSTALLER=/path/to/Graphics_SDK_setuplinux_4_03_00_02.bin' \
		'    or SDK_URL=https://... plus SDK_SHA256=<verified sha256>' \
		'  ACCEPT_TI_EULA=yes'

fetch:
	@SDK_INSTALLER="$(SDK_INSTALLER)" SDK_URL="$(SDK_URL)" \
		SDK_SHA256="$(SDK_SHA256)" OUTPUT="$(INSTALLER)" \
		sh ./scripts/fetch-sdk.sh

unpack: fetch
	@ACCEPT_TI_EULA="$(ACCEPT_TI_EULA)" INSTALLER="$(INSTALLER)" \
		OUTPUT="$(SDK_ROOT)" sh ./scripts/unpack-sdk.sh

audit: unpack
	@SDK_ROOT="$(SDK_ROOT)" OUTPUT="$(AUDIT_DIR)" sh ./scripts/audit-elf.sh

dev-tree: unpack
	@SDK_ROOT="$(SDK_ROOT)" DESTDIR="$(BUILD_DIR)/dev-tree" \
		sh ./scripts/stage-dev.sh

# Build our hard-float ABI shim (libEGL/libGLESv2 veneers + libsgxhf loader +
# libSGXm interposer) plus the libSGXm-first patched DDK. The shim .so's are our
# own code; the patched DDK under $(HF_SHIM_DIR)/ddk remains TI-proprietary.
hf-shim: unpack dev-tree
	@runtime=$$(cat "$(SDK_ROOT)/runtime.path"); \
		DDK_LIB="$$runtime" \
		DDK_INC="$(BUILD_DIR)/dev-tree/usr/include/sgx-ddk16" \
		OUT="$(HF_SHIM_DIR)" \
		CROSS_HF="$(CROSS_HF)" PATCHELF="$(PATCHELF)" \
		GEN="$(TOOLS_DIR)/gen-hf-shim.py" \
		sh "$(TOOLS_DIR)/build-hf-shim.sh"

# Reuse the 1.6-DWARF veneers (identical Khronos EGL 1.4/GLES2 ABI) to build the
# hard-float shim for the stripped DDK 1.4 sourced from its own TI SDK. The
# coverage gate fails if the 1.4 export set drifts from the veneer surface, and
# the header-diff gate (DDK_INC_REF) fails on any core ABI header change.
fetch14:
	@SDK_INSTALLER="$(SDK14_INSTALLER)" SDK_URL="$(SDK14_URL)" \
		SDK_SHA256="$(SDK14_SHA256)" OUTPUT="$(INSTALLER14)" \
		sh ./scripts/fetch-sdk.sh

unpack14: fetch14
	@ACCEPT_TI_EULA="$(ACCEPT_TI_EULA)" INSTALLER="$(INSTALLER14)" \
		OUTPUT="$(SDK14_ROOT)" sh ./scripts/unpack-sdk.sh

dev-tree14: unpack14
	@SDK_ROOT="$(SDK14_ROOT)" DESTDIR="$(BUILD_DIR)/dev-tree-14" \
		sh ./scripts/stage-dev.sh

hf-shim-14: hf-shim unpack14 dev-tree14
	@runtime=$$(cat "$(SDK14_ROOT)/runtime.path"); \
		DDK_LIB="$$runtime" \
		DDK_INC="$(BUILD_DIR)/dev-tree/usr/include/sgx-ddk16" \
		DDK_INC_REF="$(BUILD_DIR)/dev-tree-14/usr/include/sgx-ddk16" \
		OUT="$(HF_SHIM14_DIR)" \
		VENEER_SRC="$(HF_SHIM_DIR)/gen" \
		CROSS_HF="$(CROSS_HF)" PATCHELF="$(PATCHELF)" \
		GEN="$(TOOLS_DIR)/gen-hf-shim.py" \
		sh "$(TOOLS_DIR)/build-hf-shim.sh"

package: audit
	@chmod 0755 debian/rules
	@rm -rf "$(DIST_DIR)"
	@mkdir -p "$(DIST_DIR)"
	@dpkg-buildpackage --build=binary --unsigned-source --unsigned-changes --host-arch armel
	@find .. -maxdepth 1 -type f -name 'sgx-ddk16*.deb' \
		-exec mv {} "$(DIST_DIR)/" \;

verify:
	@set -e; packages=$$(find "$(DIST_DIR)" -maxdepth 1 -name '*.deb' -type f); \
		test -n "$$packages" || { echo 'no packages found in $(DIST_DIR)' >&2; exit 2; }; \
		for package in $$packages; do dpkg-deb --info "$$package"; dpkg-deb --contents "$$package"; done

clean:
	rm -rf "$(BUILD_DIR)" "$(DIST_DIR)"