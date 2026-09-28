lib.name = iemcan
# Gemeinsame Quelldateien für alle Systeme
iemcan.class.sources = iemcan.c CANreceive.c CANsend.c CANparse.c

ifeq ($(OS), Windows_NT)
	PDINCLUDEDIR ?= C:/Pd/src
    PDDIR ?= C:/Pd
	
	iemcan.class.sources += can_backend_win.c
	
	USBCAN_INC = win64/include
	USBCAN_LIB = win64/lib
	
	cflags += -D_WIN32 -I"$(USBCAN_INC)"
	ldlibs += -L"$(USBCAN_LIB)" -lUsbcan64
else
	iemcan.class.sources += can_backend_linux.c
    
    cflags += -D_GNU_SOURCE
endif

# all extra files to be included in binary distribution of the library
datafiles = \
  CANreceive-help.pd \
  CANsend-help.pd \
  CANparse-help.pd \
  $(empty)

datafiles += \
  iemcan-meta.pd \
  README.md \
  LICENSE.txt \
  $(empty)


PDLIBBUILDER_DIR = pd-lib-builder
include $(PDLIBBUILDER_DIR)/Makefile.pdlibbuilder