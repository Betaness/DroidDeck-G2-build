/*
 * src/ahb.c's AHardwareBuffer calls for tests/guards_icd_test.c, which builds the ICD without
 * libhybris: allocation fails (no swapchain images are made there).
 */
#include <android/hardware_buffer.h>

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer) {
    return -1;
}

void AHardwareBuffer_release(AHardwareBuffer* buffer) {
}

int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer* buffer, int socketFd) {
    return -1;
}
