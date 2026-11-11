#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "xf86drm.h"

int
main(void)
{
   drmDevicePtr device = NULL;
   uint32_t handle = 123;
   int prime_fd = 123;

   assert(drmGetVersion(1) == NULL);
   assert(errno == ENOSYS);
   assert(drmGetDevice2(1, 0, &device) == -1);
   assert(errno == ENOSYS && device == NULL);
   assert(drmCommandWriteRead(1, 0, NULL, 0) == -1);
   assert(errno == ENOSYS);
   assert(drmCommandWrite(1, 0, NULL, 0) == -1);
   assert(errno == ENOSYS);
   assert(drmIoctl(1, 0, NULL) == -1);
   assert(errno == ENOSYS);
   assert(drmCloseBufferHandle(1, 1) == -1);
   assert(errno == ENOSYS);
   assert(drmPrimeFDToHandle(1, 1, &handle) == -1);
   assert(errno == ENOSYS && handle == 0);
   assert(drmPrimeHandleToFD(1, 1, 0, &prime_fd) == -1);
   assert(errno == ENOSYS && prime_fd == -1);
   drmFreeDevice(&device);
   assert(device == NULL);
   return 0;
}
