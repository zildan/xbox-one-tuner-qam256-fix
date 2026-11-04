CC = i686-w64-mingw32-gcc
CFLAGS = -Wall -Wextra -O2 -s -shared -static-libgcc -Wl,--kill-at -Wl,--enable-stdcall-fixup
LIBS = -lwinusb -lsetupapi

SRCS = main_altdvb.c usb_winusb.c dib0700_core.c mn88472_core.c tda18250_core.c \
       rtl2832_core.c mn88473_core.c r820t_core.c

TARGET = Dev_DVB_Cable_Unified.int
DEF = Dev_DVB_Cable_Unified.def

all: $(TARGET)

$(TARGET): $(SRCS) $(DEF)
	$(CC) $(CFLAGS) -o $(TARGET) $(DEF) $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET)
