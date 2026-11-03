CC = x86_64-w64-mingw32-gcc
DLLTOOL = x86_64-w64-mingw32-dlltool
CFLAGS = -Wall -Wextra -O2 -nostdlib -I. -I./include -D_WIN32_WINNT=0x0502
LDFLAGS = -shared -nostdlib -Wl,-e,DrvEnableDriver -Wl,--subsystem,native:5.2 -L. -lwin32k
HOSTCC ?= cc

OBJS = enable.o screen.o blt.o text.o

all: nvxdisp.dll

libwin32k.a: win32k.def
	$(DLLTOOL) -d $< -l $@

nvxdisp.dll: $(OBJS) libwin32k.a
	$(CC) $(OBJS) $(LDFLAGS) -o $@

%.o: %.c driver.h color.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) nvxdisp.dll libwin32k.a

check:
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -I. tests/color_test.c -o /tmp/nvx-color-test
	/tmp/nvx-color-test

.PHONY: all clean check
