#################################################################
## BUSYBOX                                                     ##
#################################################################

BUSYBOXVERSION := 1.38.0
BUSYBOXARCHIVE := busybox-$(BUSYBOXVERSION).tar.bz2
BUSYBOXURI     := https://busybox.net/downloads/$(BUSYBOXARCHIVE)

# The toolchain (gcc 4.4) is a 32-bit host binary which cannot read
# sources located on Windows drvfs (/mnt/c) in WSL, failing with
# "Value too large for defined data type". Stage the build on native
# filesystem and copy the resulting artifacts back.
BUSYBOXNATIVEDIR := $(or $(MIICAM_NATIVEDIR),$(HOME)/.miicam-build)
BUSYBOXBIN       := $(PREFIXDIR)/bin/busybox


#################################################################
##                                                             ##
#################################################################

$(SOURCEDIR)/$(BUSYBOXARCHIVE): $(SOURCEDIR)
	$(DOWNLOADCMD) $@ $(BUSYBOXURI) || rm -f $@


#################################################################
##                                                             ##
#################################################################

$(BUILDDIR)/busybox: $(SOURCEDIR)/$(BUSYBOXARCHIVE)
	@mkdir -p $(BUILDDIR) $(PREFIXDIR)/bin $(BUSYBOXNATIVEDIR) && rm -rf $(BUSYBOXNATIVEDIR)/busybox-$(BUSYBOXVERSION)
	@tar -xjf $(SOURCEDIR)/$(BUSYBOXARCHIVE) -C $(BUSYBOXNATIVEDIR)
	@cd $(BUSYBOXNATIVEDIR)/busybox-$(BUSYBOXVERSION)		&& \
	echo "*** Configuring busybox $(BUSYBOXVERSION)"		&& \
	$(BUILDENV) make -j$(PROCS) defconfig				&& \
	echo "*** Enabling all busybox applets"				&& \
	for app in $$(grep -rhoE "//applet:IF_[A-Z0-9_]+\(" --include="*.c" . | sed -E 's/.*IF_([A-Z0-9_]+)\(/\1/' | sort -u); do grep -rq "^config $$app$$" --include="Config.in" . && sed -i -e "s/^# CONFIG_$$app is not set/CONFIG_$$app=y/" .config || true; done && \
	sed -i -e 's/^CONFIG_SH_IS_HUSH=y/CONFIG_SH_IS_HUSH=n/' -e 's/^CONFIG_BASH_IS_HUSH=y/CONFIG_BASH_IS_HUSH=n/' -e 's/^CONFIG_BASH_IS_NONE=y/CONFIG_BASH_IS_NONE=n/' -e 's/^# CONFIG_BASH_IS_ASH is not set/CONFIG_BASH_IS_ASH=y/' .config && \
	sed -i -e 's/^CONFIG_TIME64=y/CONFIG_TIME64=n/' .config	&& \
	sed -i -e 's|^CONFIG_CROSS_COMPILER_PREFIX=""|CONFIG_CROSS_COMPILER_PREFIX="$(TARGET)-"|' .config && \
	sed -i -e 's/^CONFIG_TC=y/CONFIG_TC=n/' -e 's/^CONFIG_USE_BB_CRYPT_YES=y/CONFIG_USE_BB_CRYPT_YES=n/' -e 's/^CONFIG_UDHCPC6=y/CONFIG_UDHCPC6=n/' -e 's/^CONFIG_SEEDRNG=y/CONFIG_SEEDRNG=n/' -e 's/^CONFIG_NSENTER=y/CONFIG_NSENTER=n/' -e 's/^CONFIG_FEATURE_SYNC_FANCY=y/CONFIG_FEATURE_SYNC_FANCY=n/' .config && \
	$(BUILDENV) make -j$(PROCS) oldconfig				&& \
	echo "*** Building busybox $(BUSYBOXVERSION)"			&& \
	$(BUILDENV) make -j$(PROCS)					&& \
	echo "*** Installing busybox $(BUSYBOXVERSION)"			&& \
	$(BUILDENV) make -j$(PROCS) install CONFIG_PREFIX=$(BUSYBOXNATIVEDIR)/busybox-install && \
	cp -f $(BUSYBOXNATIVEDIR)/busybox-$(BUSYBOXVERSION)/busybox $(BUSYBOXBIN) && \
	cp -f $(BUSYBOXNATIVEDIR)/busybox-$(BUSYBOXVERSION)/busybox.links $(PREFIXDIR)/busybox.links
	@rm -rf $(BUSYBOXNATIVEDIR)/busybox-$(BUSYBOXVERSION) $(BUSYBOXNATIVEDIR)/busybox-install
	@touch $@


#################################################################
##                                                             ##
#################################################################
