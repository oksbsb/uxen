TOPLEVEL=$(CURDIR)/../..
XEN_ROOT=$(TOPLEVEL)/../..
include $(TOPLEVEL)/common.make

CFLAGS += -I../mmap $(CFLAGS_libxenctrl) $(CFLAGS_libxenguest)
OCAMLINCLUDE += -I ../mmap

OBJS = xenctrl
INTF = xenctrl.cmi
LIBS = xenctrl.cma xenctrl.cmxa

LIBS_xenctrl = $(LDLIBS_libxenctrl) $(LDLIBS_libxenguest)

xenctrl_OBJS = $(OBJS)
xenctrl_C_OBJS = xenctrl_stubs

OCAML_LIBRARY = xenctrl

all: $(INTF) $(LIBS)

libs: $(LIBS)

.PHONY: install
install: $(LIBS) META
	mkdir -p $(OCAMLDESTDIR)
	ocamlfind remove -destdir $(OCAMLDESTDIR) xenctrl
	ocamlfind install -destdir $(OCAMLDESTDIR) -ldconf ignore xenctrl META $(INTF) $(LIBS) *.a *.so *.cmx

.PHONY: uninstall
uninstall:
	ocamlfind remove -destdir $(OCAMLDESTDIR) xenctrl

include $(TOPLEVEL)/Makefile.rules
