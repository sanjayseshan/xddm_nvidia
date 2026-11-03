CC = x86_64-w64-mingw32-gcc
CFLAGS = -Wall -Wextra -O2 -nostdlib -I. -I./include -D_WIN32_WINNT=0x0502
LDFLAGS = -shared -nostdlib -Wl,-e,DrvEnableDriver -Wl,--subsystem,native:5.2 -L../vendor/win32k -lwin32k

OBJS = enable.o screen.o blt.o text.o

all: nvxdisp.dll

nvxdisp.dll: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

%.o: %.c driver.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) nvxdisp.dll

.PHONY: all clean
