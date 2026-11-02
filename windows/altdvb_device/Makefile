CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -s -static-libgcc -shared -Wl,--kill-at -Wl,--enable-stdcall-fixup
LIBS = -lwinusb -lsetupapi

SRCS = main_altdvb.c dib0700_core.c mn88472_core.c tda18250_core.c usb_winusb.c
TARGET = Dev_XboxOne_QAM256.int
DEF = Dev_XboxOne_QAM256.def

all: $(TARGET)

$(TARGET): $(SRCS) $(DEF)
	$(CC) $(CFLAGS) -o $@ $(DEF) $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET)
