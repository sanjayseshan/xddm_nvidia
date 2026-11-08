CC = x86_64-w64-mingw32-gcc
CC32 = i686-w64-mingw32-gcc
DLLTOOL = x86_64-w64-mingw32-dlltool
CFLAGS = -Wall -Wextra -O2 -nostdlib -I. -I./include -D_WIN32_WINNT=0x0502
CFLAGS32 = -Wall -Wextra -O2 -nostdlib -I. -I./include -D_WIN32_WINNT=0x0501 -m32
LDFLAGS = -shared -nostdlib -Wl,-e,DrvEnableDriver -Wl,--subsystem,native:5.2 -L. -lwin32k -lntoskrnl
LDFLAGS32 = -shared -nostdlib -Wl,-e,_DrvEnableDriver@12 -Wl,--subsystem,native:5.1 /usr/i686-w64-mingw32/lib/libwin32k.a /usr/i686-w64-mingw32/lib/libntoskrnl.a -lgcc
HOSTCC ?= cc

OBJS = enable.o screen.o blt.o text.o diagnostic.o opengl.o
OBJS32 = enable32.o screen32.o blt32.o text32.o diagnostic32.o opengl32.o

all: nvxdisp.dll nvxdisp32.dll

libwin32k.a: win32k.def
	$(DLLTOOL) -d $< -l $@

libntoskrnl.a: ntoskrnl.def
	$(DLLTOOL) -d $< -l $@

nvxdisp.dll: $(OBJS) libwin32k.a libntoskrnl.a
	$(CC) $(OBJS) $(LDFLAGS) -o $@

nvxdisp32.dll: $(OBJS32)
	$(CC32) $(OBJS32) $(LDFLAGS32) -o $@

%.o: %.c driver.h color.h
	$(CC) $(CFLAGS) -c $< -o $@

%32.o: %.c driver.h color.h
	$(CC32) $(CFLAGS32) -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS32) nvxdisp.dll nvxdisp32.dll libwin32k.a libntoskrnl.a

check:
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -I. tests/color_test.c -o /tmp/nvx-color-test
	/tmp/nvx-color-test

.PHONY: all clean check
