#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef char SMCBytes_t[32];
typedef struct {
    uint32_t key;
    char     vers_major, vers_minor, vers_build, vers_reserved;
    uint16_t vers_release;
    uint16_t pLimit_version;
    uint16_t pLimit_length;
    uint32_t cpuPLimit, gpuPLimit, memPLimit;
    uint32_t keyInfo_dataSize;
    uint32_t keyInfo_dataType;
    char     keyInfo_dataAttributes;
    char     result, status;
    char     data8;
    uint32_t data32;
    SMCBytes_t bytes;
} SMCKeyData_t;

int main(void) {
    printf("sizeof(SMCKeyData_t)=%zu\n", sizeof(SMCKeyData_t));
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    printf("svc=%u\n", svc);
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    printf("open kr=%d conn=%u\n", kr, conn);

    SMCKeyData_t in, out;
    size_t outSize = sizeof(SMCKeyData_t);
    memset(&in, 0, sizeof(in));
    in.data8 = 1; /* SMC_CMD_GET_KEY_COUNT */
    kr = IOConnectCallStructMethod(conn, 2, &in, sizeof(in), &out, &outSize);
    printf("count kr=%d dataSize=%u data32=%u result=%d status=%d\n",
           kr, out.keyInfo_dataSize, out.data32, out.result, out.status);

    memset(&in, 0, sizeof(in));
    in.data8 = 2; /* SMC_CMD_GET_KEY_INDEX */
    in.data32 = 0;
    outSize = sizeof(SMCKeyData_t);
    kr = IOConnectCallStructMethod(conn, 2, &in, sizeof(in), &out, &outSize);
    printf("idx kr=%d key=%08x (%c%c%c%c)\n", kr, out.key,
           (char)(out.key>>24), (char)(out.key>>16), (char)(out.key>>8), (char)out.key);

    /* FNum keyinfo */
    memset(&in, 0, sizeof(in));
    in.key = ((uint32_t)'F'<<24)|((uint32_t)'N'<<16)|((uint32_t)'u'<<8)|(uint32_t)'m';
    in.data8 = 9; /* SMC_CMD_READ_KEYINFO */
    outSize = sizeof(SMCKeyData_t);
    kr = IOConnectCallStructMethod(conn, 2, &in, sizeof(in), &out, &outSize);
    printf("FNum info kr=%d size=%u type=%08x\n", kr, out.keyInfo_dataSize, out.keyInfo_dataType);

    /* FNum read */
    memset(&in, 0, sizeof(in));
    in.key = ((uint32_t)'F'<<24)|((uint32_t)'N'<<16)|((uint32_t)'u'<<8)|(uint32_t)'m';
    in.data8 = 5; /* SMC_CMD_READ_BYTES */
    in.keyInfo_dataSize = 1;
    outSize = sizeof(SMCKeyData_t);
    kr = IOConnectCallStructMethod(conn, 2, &in, sizeof(in), &out, &outSize);
    printf("FNum read kr=%d byte0=%u\n", kr, (uint8_t)out.bytes[0]);
    return 0;
}
