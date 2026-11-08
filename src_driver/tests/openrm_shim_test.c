#include "host_videoport.h"
#include "nvx_openrm_shim.h"
#include "nvx_gsp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

ULONG VideoPortReadRegisterUlong(PULONG p)
{
    return *p;
}

VOID VideoPortWriteRegisterUlong(PULONG p, ULONG v)
{
    *p = v;
}

UCHAR VideoPortReadRegisterUchar(PUCHAR p)
{
    return *p;
}

VOID VideoPortWriteRegisterUchar(PUCHAR p, UCHAR v)
{
    *p = v;
}

VOID VideoPortMoveMemory(PVOID d, PVOID s, ULONG l)
{
    memmove(d, s, l);
}

VOID VideoPortZeroMemory(PVOID d, ULONG l)
{
    memset(d, 0, l);
}

VOID VideoPortStallExecution(ULONG us)
{
    (void)us;
}

static void test_register_access(void)
{
    uint32_t fake_bar0[1024];
    memset(fake_bar0, 0, sizeof(fake_bar0));

    GPU_REG_WR32(fake_bar0, 0x100, 0x12345678);
    assert(GPU_REG_RD32(fake_bar0, 0x100) == 0x12345678);

    GPU_REG_WR08(fake_bar0, 0x200, 0xAB);
    assert(GPU_REG_RD08(fake_bar0, 0x200) == 0xAB);

    /* Null base safety */
    assert(GPU_REG_RD32(NULL, 0x100) == 0xFFFFFFFFU);
    assert(GPU_REG_RD08(NULL, 0x100) == 0xFFU);
    GPU_REG_WR32(NULL, 0x100, 0x42);
    GPU_REG_WR08(NULL, 0x100, 0x42);
}

static void test_memory_ops(void)
{
    char src[32] = "OpenRM Unified Hardware HAL";
    char dst[32];
    memset(dst, 0, sizeof(dst));

    nvx_mem_copy(dst, src, strlen(src) + 1);
    assert(strcmp(dst, src) == 0);

    nvx_mem_zero(dst, sizeof(dst));
    for (size_t i = 0; i < sizeof(dst); i++) {
        assert(dst[i] == 0);
    }
}

static void test_msgq_notification(void)
{
    NVX_DEVICE_EXTENSION dev;
    uint32_t fake_bar0[0x111000 / 4];
    memset(&dev, 0, sizeof(dev));
    memset(fake_bar0, 0, sizeof(fake_bar0));

    dev.Bar0MmioVirtual = fake_bar0;

    /* Read notify should be no-op */
    assert(nvx_msgq_notify_remote(FCN_FLAG_NOTIFY_MSG_READ, &dev) == 0);

    /* Write notify should ring doorbell at 0x110c00 */
    assert(nvx_msgq_notify_remote(FCN_FLAG_NOTIFY_MSG_WRITE, &dev) == 0);
    assert(fake_bar0[0x110c00 / 4] == 0);
}

static void test_falcon_idle(void)
{
    uint32_t fake_bar0[0x200 / 4];
    memset(fake_bar0, 0, sizeof(fake_bar0));

    /* Idle condition: dmactl scrubbing bits 0x6 are clear */
    fake_bar0[0x10c / 4] = 0x0;
    assert(nvx_falcon_wait_idle(fake_bar0, 0, 100) == NV_TRUE);

    /* Busy condition: bit 1 set (scrubbing in progress) */
    fake_bar0[0x10c / 4] = 0x2;
    assert(nvx_falcon_wait_idle(fake_bar0, 0, 50) == NV_FALSE);
}

int main(void)
{
    test_register_access();
    test_memory_ops();
    test_msgq_notification();
    test_falcon_idle();
    printf("OpenRM HAL shim tests passed: MMIO, memory, MsgQ doorbell, Falcon idle.\n");
    return 0;
}
