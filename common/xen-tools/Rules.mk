#  -*- mode: Makefile; -*-

# `all' is the default target
all:

include $(XEN_TOOLS_ROOT)/Config.mk

export _INSTALL := $(INSTALL)
INSTALL = $(XEN_TOOLS_ROOT)/cross-install

XEN_INCLUDE        = $(XEN_TOOLS_ROOT)/../include/xen-public
XEN_LIBXC          = $(XEN_TOOLS_ROOT)/libxc
XEN_XENLIGHT       = $(XEN_TOOLS_ROOT)/libxl
XEN_XENSTORE       = $(XEN_TOOLS_ROOT)/xenstore
XEN_LIBXENSTAT     = $(XEN_TOOLS_ROOT)/xenstat/libxenstat/src
XEN_BLKTAP2        = $(XEN_TOOLS_ROOT)/blktap2
XEN_LIBVCHAN       = $(XEN_TOOLS_ROOT)/libvchan

CFLAGS_xeninclude = -I$(XEN_INCLUDE)

CFLAGS_libxenctrl = -I$(XEN_LIBXC) $(CFLAGS_xeninclude)
LDLIBS_libxenctrl = $(XEN_LIBXC)/libxenctrl.so
SHLIB_libxenctrl  = -Wl,-rpath-link=$(XEN_LIBXC)

CFLAGS_libxenguest = -I$(XEN_LIBXC) $(CFLAGS_xeninclude)
LDLIBS_libxenguest = $(XEN_LIBXC)/libxenguest.so
SHLIB_libxenguest  = -Wl,-rpath-link=L$(XEN_LIBXC)

CFLAGS_libxenstore = -I$(XEN_XENSTORE) $(CFLAGS_xeninclude)
LDLIBS_libxenstore = $(XEN_XENSTORE)/libxenstore.so
SHLIB_libxenstore  = -Wl,-rpath-link=$(XEN_XENSTORE)

CFLAGS_libxenstat  = -I$(XEN_LIBXENSTAT)
LDLIBS_libxenstat  = $(SHLIB_libxenctrl) $(SHLIB_libxenstore) $(XEN_LIBXENSTAT)/libxenstat.so
SHLIB_libxenstat  = -Wl,-rpath-link=$(XEN_LIBXENSTAT)

CFLAGS_libxenvchan = -I$(XEN_LIBVCHAN)
LDLIBS_libxenvchan = $(SHLIB_libxenctrl) $(SHLIB_libxenstore) -L$(XEN_LIBVCHAN) -lxenvchan
SHLIB_libxenvchan  = -Wl,-rpath-link=$(XEN_LIBVCHAN)

ifeq ($(CONFIG_Linux),y)
LIBXL_BLKTAP = y
else
LIBXL_BLKTAP = n
endif

ifeq ($(LIBXL_BLKTAP),y)
CFLAGS_libblktapctl = -I$(XEN_BLKTAP2)/control -I$(XEN_BLKTAP2)/include $(CFLAGS_xeninclude)
LDLIBS_libblktapctl = -L$(XEN_BLKTAP2)/control -lblktapctl
SHLIB_libblktapctl  = -Wl,-rpath-link=$(XEN_BLKTAP2)/control
else
CFLAGS_libblktapctl =
LDLIBS_libblktapctl =
SHLIB_libblktapctl  =
endif

CFLAGS_libxenlight = -I$(XEN_XENLIGHT) $(CFLAGS_libxenctrl) $(CFLAGS_xeninclude)
LDLIBS_libxenlight = $(XEN_XENLIGHT)/libxenlight.so $(SHLIB_libxenctrl) $(SHLIB_libxenstore) $(SHLIB_libblktapctl)
SHLIB_libxenlight  = -Wl,-rpath-link=$(XEN_XENLIGHT)

CFLAGS += -D__XEN_TOOLS__

# Get gcc to generate the dependencies for us.
CFLAGS += -MMD -MF .$(@F).d
DEPS = .*.d

#ifneq ($(XEN_OS),NetBSD)
## Enable implicit LFS support *and* explicit LFS names.
#CFLAGS  += $(shell getconf LFS_CFLAGS)
#CFLAGS  += -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
#LDFLAGS += $(shell getconf LFS_LDFLAGS)
#endif

# 32-bit x86 does not perform well with -ve segment accesses on Xen.
CFLAGS-$(CONFIG_X86_32) += $(call cc-option,$(CC),-mno-tls-direct-seg-refs)
CFLAGS += $(CFLAGS-y)

# Require GCC v3.4+ (to avoid issues with alignment constraints in Xen headers)
check-$(CONFIG_X86) = $(call cc-ver-check,CC,0x030400,\
                        "Xen requires at least gcc-3.4")
$(eval $(check-y))

_PYTHON_PATH := $(shell which $(PYTHON))
PYTHON_PATH ?= $(_PYTHON_PATH)
INSTALL_PYTHON_PROG = \
	$(XEN_TOOLS_ROOT)/python/install-wrap "$(PYTHON_PATH)" $(INSTALL_PROG)

%.opic: %.c
	$(CC) $(CPPFLAGS) -DPIC $(CFLAGS) $(CFLAGS_$*.opic) -fPIC -c -o $@ $< $(APPEND_CFLAGS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_$*.o) -c -o $@ $< $(APPEND_CFLAGS)

%.o: %.cc
	$(CC) $(CPPFLAGS) $(CXXFLAGS) $(CXXFLAGS_$*.o) -c -o $@ $< $(APPEND_CFLAGS)

%.o: %.S
	$(CC) $(CFLAGS) $(CFLAGS_$*.o) -c $< -o $@ $(APPEND_CFLAGS)
%.opic: %.S
	$(CC) $(CPPFLAGS) -DPIC $(CFLAGS) $(CFLAGS.opic) -fPIC -c -o $@ $< $(APPEND_CFLAGS)
