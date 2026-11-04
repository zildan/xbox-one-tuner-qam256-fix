CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -s -static-libgcc -shared -Wl,--kill-at -Wl,--enable-stdcall-fixup -I.
LIBS = -lwinusb -lsetupapi -lm

SRCS = main_altdvb.c rtl2832_core.c mn88473_core.c r820t_core.c usb_winusb.c
TARGET = Dev_Astrometa_DVB.int
DEF = Dev_Astrometa_DVB.def

all: $(TARGET)

$(TARGET): $(SRCS) $(DEF)
	$(CC) $(CFLAGS) -o $@ $(DEF) $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET)
