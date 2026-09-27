SHELL := /bin/sh

# DDK 1.4 build selector: 2616 (TI Graphics SDK 4.00.00.01) or 2514 (3.01.00.02).
DDK_BUILD ?= 2616

ifeq ($(DDK_BUILD),2616)
SDK_VERSION := 4.00.00.01
DDK_VERSION := 1.4.14.2616
INSTALLER_NAME := Graphics_SDK_setuplinux_4_00_00_01.bin
UNPACK_PAGED_EULA := no
else ifeq ($(DDK_BUILD),2514)
SDK_VERSION := 3.01.00.02
DDK_VERSION := 1.4.14.2514
INSTALLER_NAME := OMAP35x_Graphics_SDK_setuplinux_3_01_00_02.bin
UNPACK_PAGED_EULA := yes
else
$(error unknown DDK_BUILD '$(DDK_BUILD)'; use 2616 or 2514)
endif

BUILD_DIR ?= $(CURDIR)/build
DIST_DIR ?= $(CURDIR)/dist
SDK_INSTALLER ?=
SDK_URL ?=
SDK_SHA256 ?=
ACCEPT_TI_EULA ?= no
INSTALLER := $(BUILD_DIR)/downloads/$(INSTALLER_NAME)
SDK_ROOT := $(BUILD_DIR)/sdk
AUDIT_DIR := $(BUILD_DIR)/audit

.DEFAULT_GOAL := help

.PHONY: help fetch unpack audit dev-tree package verify clean

help:
	@printf '%s\n' \
		'SGX DDK 1.4 userspace package pipeline (DDK_BUILD=$(DDK_BUILD) -> $(DDK_VERSION))' \
		'' \
		'  make fetch    Download or copy the checksum-pinned TI installer' \
		'  make unpack   Extract gfx_rel_es2.x after explicit EULA acceptance' \
		'  make audit    Record ELF attributes, dependencies, imports, and exports' \
		'  make dev-tree Stage public TI headers and pkg-config metadata' \
		'  make package  Build the runtime, tools, and development packages' \
		'  make verify   Inspect package contents and run static ELF checks' \
		'' \
		'Select the build with DDK_BUILD=2616 (SDK 4.00.00.01) or 2514 (SDK 3.01.00.02).' \
		'The 2514 installer pages its EULA, so unpack needs python3 + python3-pexpect.' \
		'' \
		'Required inputs:' \
		'  SDK_INSTALLER=/path/to/$(INSTALLER_NAME)' \
		'    or SDK_URL=https://... plus SDK_SHA256=<verified sha256>' \
		'  ACCEPT_TI_EULA=yes'

fetch:
	@SDK_INSTALLER="$(SDK_INSTALLER)" SDK_URL="$(SDK_URL)" \
		SDK_SHA256="$(SDK_SHA256)" OUTPUT="$(INSTALLER)" \
		sh ./scripts/fetch-sdk.sh

unpack: fetch
	@ACCEPT_TI_EULA="$(ACCEPT_TI_EULA)" INSTALLER="$(INSTALLER)" \
		UNPACK_PAGED_EULA="$(UNPACK_PAGED_EULA)" \
		OUTPUT="$(SDK_ROOT)" sh ./scripts/unpack-sdk.sh

audit: unpack
	@SDK_ROOT="$(SDK_ROOT)" OUTPUT="$(AUDIT_DIR)" sh ./scripts/audit-elf.sh

dev-tree: unpack
	@SDK_ROOT="$(SDK_ROOT)" DESTDIR="$(BUILD_DIR)/dev-tree" \
		sh ./scripts/stage-dev.sh

package: audit
	@sed -i '1s/(1\.4\.14\.[0-9][0-9]*)/($(DDK_VERSION))/' debian/changelog
	@chmod 0755 debian/rules
	@rm -rf "$(DIST_DIR)"
	@mkdir -p "$(DIST_DIR)"
	@dpkg-buildpackage --build=binary --unsigned-source --unsigned-changes --host-arch armel
	@find .. -maxdepth 1 -type f -name 'sgx-ddk14*.deb' \
		-exec mv {} "$(DIST_DIR)/" \;

verify:
	@set -e; packages=$$(find "$(DIST_DIR)" -maxdepth 1 -name '*.deb' -type f); \
		test -n "$$packages" || { echo 'no packages found in $(DIST_DIR)' >&2; exit 2; }; \
		for package in $$packages; do dpkg-deb --info "$$package"; dpkg-deb --contents "$$package"; done

clean:
	rm -rf "$(BUILD_DIR)" "$(DIST_DIR)"
