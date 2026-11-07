# Open Watcom 2 environment for building NVX Win98 Display Driver
: "${WATCOM:=/home/sanjay/watcom}"
export WATCOM
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h:$WATCOM/h/win"
export EDPATH="$WATCOM/eddat"
export LIB="$WATCOM/lib286:$WATCOM/lib286/win"
