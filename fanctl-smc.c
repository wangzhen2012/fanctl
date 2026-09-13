/*
 * fanctl-smc — AppleSMC 读写工具（macOS Apple Silicon, arm64）
 *
 * 结构体采用与 iFan 相同的新版 80 字节 Swift 布局（反汇编实测）:
 *   data8(命令字节)@0x2a  dataSize@0x1c  dataType@0x20  bytes@0x30
 * 旧版 76 字节 C 布局在 macOS 26 / M4 上返回 kIOReturnBadArgument。
 *
 * 用法:
 *   fanctl-smc fans                列出风扇（id 当前 最小 最大 模式 目标）
 *   fanctl-smc temps               列出温度类 key
 *   fanctl-smc temp                输出最高温度（℃）
 *   fanctl-smc get KEY             读任意 4 字符 key
 *   fanctl-smc set KEY VALUE       写任意 key（int/float 按类型）
 *   fanctl-smc fan -m 0|1         设置风扇模式 (0=自动 1=手动)
 *   fanctl-smc fan -v N           设置转速
 *   fanctl-smc fan -m 1 -v N      手动模式并设转速
 *   fanctl-smc reset              恢复自动
 *
 * 读无需特权；写需要 root（配合 sudoers NOPASSWD）。
 */
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

typedef struct {
    uint32_t key;         /* 0x00 */
    char     vers[6];     /* 0x04 */
    uint16_t plim_ver;    /* 0x0a */
    uint16_t plim_len;    /* 0x0c */
    uint32_t cpuPLimit;   /* 0x10 */
    uint32_t gpuPLimit;   /* 0x14 */
    uint32_t memPLimit;   /* 0x18 */
    uint32_t dataSize;    /* 0x1c */
    uint32_t dataType;    /* 0x20 */
    char     dataAttrs;   /* 0x24 */
    char     result;      /* 0x25 */
    char     status;      /* 0x26 */
    char     pad1[3];     /* 0x27-0x29 */
    char     data8;       /* 0x2a */
    char     pad2;        /* 0x2b */
    uint32_t data32;      /* 0x2c */
    char     bytes[32];   /* 0x30 */
} SMCKeyData;

static io_connect_t g_conn = 0;

static uint32_t fourcc(const char *s) {
    return ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) |
           ((uint32_t)s[2] << 8)  | ((uint32_t)s[3]);
}

static void deccfour(uint32_t k, char out[5]) {
    out[0] = (char)((k >> 24) & 0xff);
    out[1] = (char)((k >> 16) & 0xff);
    out[2] = (char)((k >> 8) & 0xff);
    out[3] = (char)(k & 0xff);
    out[4] = 0;
}

static int smc_open(void) {
    if (g_conn) return 0;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("AppleSMC"));
    if (!svc) { fprintf(stderr, "[ERROR] AppleSMC not found\n"); return -1; }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &g_conn);
    IOObjectRelease(svc);
    if (kr != kIOReturnSuccess) { fprintf(stderr, "[ERROR] open: %d\n", kr); return -1; }
    return 0;
}

static int smc_call(SMCKeyData *in, SMCKeyData *out) {
    size_t outSize = sizeof(SMCKeyData);
    memset(out, 0, sizeof(SMCKeyData));
    kern_return_t kr = IOConnectCallStructMethod(g_conn, 2, in, sizeof(SMCKeyData),
                                                 out, &outSize);
    if (kr != kIOReturnSuccess) return -1;
    if (out->result != 0) return -2;
    return 0;
}

static int smc_keyinfo(const char *key, uint32_t *size, uint32_t *type) {
    SMCKeyData in, out;
    memset(&in, 0, sizeof(in));
    in.key = fourcc(key);
    in.data8 = 9; /* READ_KEYINFO */
    if (smc_call(&in, &out) != 0) return -1;
    *size = out.dataSize;
    *type = out.dataType;
    return 0;
}

static int smc_read(const char *key, char *buf, uint32_t *size) {
    uint32_t ksize, ktype;
    if (smc_keyinfo(key, &ksize, &ktype) != 0) return -1;
    if (ksize == 0) return -2; /* key 不存在/无数据 */
    SMCKeyData in, out;
    memset(&in, 0, sizeof(in));
    in.key = fourcc(key);
    in.dataSize = ksize;
    in.data8 = 5; /* READ_BYTES */
    if (smc_call(&in, &out) != 0) return -1;
    memcpy(buf, out.bytes, ksize);
    *size = ksize;
    return 0;
}

static int smc_write(const char *key, const char *val, uint32_t size, uint32_t type) {
    SMCKeyData in, out;
    memset(&in, 0, sizeof(in));
    in.key = fourcc(key);
    in.dataSize = size;
    in.dataType = type;
    in.data8 = 6; /* WRITE_BYTES */
    memcpy(in.bytes, val, size);
    return smc_call(&in, &out);
}

/* ---------- 类型工具 ---------- */

static uint32_t t_flt = 0x666c7420; /* 'flt ' */
static uint32_t t_ui8 = 0x75693820; /* 'ui8 ' */
static uint32_t t_ui16 = 0x75693136; /* 'ui16' */
static uint32_t t_ui32 = 0x75693332; /* 'ui32' */
static uint32_t t_si8 = 0x73693820; /* 'si8 ' */

static int is_fp_type(uint32_t t) {
    char s[5]; deccfour(t, s);
    return (s[0] == 'f' || s[0] == 's') && s[1] == 'p';
}

/* 解码 SMC 值到字符串 */
static int decode(const char *key, const char *b, uint32_t size, uint32_t type,
                  char *out, size_t outlen) {
    if (type == t_flt && size == 4) {
        float f; memcpy(&f, b, 4);
        snprintf(out, outlen, "%g", f); return 0;
    }
    if (type == t_ui8 && size == 1) {
        snprintf(out, outlen, "%u", (uint8_t)b[0]); return 0;
    }
    if (type == t_ui16 && size == 2) {
        snprintf(out, outlen, "%u", (uint16_t)((b[0]<<8)|b[1])); return 0;
    }
    if (type == t_ui32 && size == 4) {
        uint32_t v; memcpy(&v, b, 4);
        snprintf(out, outlen, "%u", v); return 0;
    }
    if (type == t_si8 && size == 1) {
        snprintf(out, outlen, "%d", (int8_t)b[0]); return 0;
    }
    if (is_fp_type(type)) {
        char s[5]; deccfour(type, s);
        int frac = s[3] - '0';
        if (size == 2) {
            int16_t v = (int16_t)((b[0]<<8)|b[1]);
            snprintf(out, outlen, "%.3f", v / (double)(1 << frac)); return 0;
        }
        if (size == 4) {
            int32_t v; memcpy(&v, b, 4);
            snprintf(out, outlen, "%.3f", v / (double)(1 << frac)); return 0;
        }
    }
    /* 原始 hex */
    snprintf(out, outlen, "0x");
    for (uint32_t i = 0; i < size && i < 8; i++) {
        char t[4]; snprintf(t, sizeof(t), "%02x", (uint8_t)b[i]);
        strncat(out, t, outlen - strlen(out) - 1);
    }
    return 0;
}

/* 编码用户输入到字节（按 key 类型） */
static int encode(const char *val, char *b, uint32_t size, uint32_t type) {
    memset(b, 0, size > 32 ? 32 : size);
    if (type == t_flt && size == 4) {
        float f = (float)strtod(val, NULL);
        memcpy(b, &f, 4); return 0;
    }
    if (type == t_ui8 && size == 1) {
        b[0] = (char)strtoul(val, NULL, 0); return 0;
    }
    if (type == t_ui16 && size == 2) {
        uint16_t v = (uint16_t)strtoul(val, NULL, 0);
        b[0] = (char)(v >> 8); b[1] = (char)(v & 0xff); return 0;
    }
    if (type == t_ui32 && size == 4) {
        uint32_t v = (uint32_t)strtoul(val, NULL, 0);
        memcpy(b, &v, 4); return 0;
    }
    if (is_fp_type(type)) {
        char s[5]; deccfour(type, s);
        int frac = s[3] - '0';
        double d = strtod(val, NULL);
        if (size == 2) {
            int16_t v = (int16_t)lround(d * (1 << frac));
            b[0] = (char)(v >> 8); b[1] = (char)(v & 0xff); return 0;
        }
        if (size == 4) {
            int32_t v = (int32_t)lround(d * (1 << frac));
            memcpy(b, &v, 4); return 0;
        }
    }
    return -1;
}

/* ---------- 命令 ---------- */

static void cmd_fans(void) {
    char b[32]; uint32_t size;
    int nfans = 0;
    if (smc_read("FNum", b, &size) == 0) nfans = (uint8_t)b[0];
    printf("fans %d\n", nfans);
    for (int i = 0; i < nfans; i++) {
        char key[5];
        double cur = 0, mn = 0, mx = 0, tg = 0; int md = -1;
        snprintf(key, 5, "F%dAc", i);
        if (smc_read(key, b, &size) == 0 && size == 4) { float f; memcpy(&f, b, 4); cur = f; }
        snprintf(key, 5, "F%dMn", i);
        if (smc_read(key, b, &size) == 0 && size == 4) { float f; memcpy(&f, b, 4); mn = f; }
        snprintf(key, 5, "F%dMx", i);
        if (smc_read(key, b, &size) == 0 && size == 4) { float f; memcpy(&f, b, 4); mx = f; }
        snprintf(key, 5, "F%dMd", i);
        if (smc_read(key, b, &size) == 0 && size == 1) md = (uint8_t)b[0];
        snprintf(key, 5, "F%dTg", i);
        if (smc_read(key, b, &size) == 0 && size == 4) { float f; memcpy(&f, b, 4); tg = f; }
        printf("fan %d cur=%.0f min=%.0f max=%.0f mode=%d target=%.0f\n",
               i, cur, mn, mx, md, tg);
    }
}

/* 温度 key 列表（Apple Silicon M4） */
static const char *TEMP_KEYS[] = {
    "Tp09","Tp0C","Tp0D","Tp0E","Tp1A","Tp1B","Tp1C","Tp1E","Tp1F",
    "TCMb","TCMz","TH0a","TH0b","TH0p","TH0x","TIED","TMVR",
    "TPD0","TPD1","TPD2","TPD3","TPD4","TPD5","TPD6","TPD7","TPD8","TPD9",
    "TPDA","TPDB","TPDC","TPDD","TPDE","TPDF","TPDX",
    "TPSD","TPSP","TRD0","TRD1","TRD2","TRD3","TRD4","TRD5","TRD6","TRD7","TRDX",
    "TSCD","TT0P","TUVR","TVA0"
};

/* 稳定温度源（读数平滑，适合做控制信号；TCMz 是瞬时热点会剧烈抖动） */
static const char *STABLE_TEMP_KEYS[] = {
    "Tp1C", "Tp0E", "TCMb", "Tp09", "Tp0D", "Tp1B", "Tp1F", "Tp1E", "Tp0C", "Tp1A"
};

static int read_float_key(const char *key, float *out) {
    uint32_t size, type;
    char b[32];
    if (smc_keyinfo(key, &size, &type) != 0 || size != 4) return -1;
    if (type != t_flt) return -1;
    if (smc_read(key, b, &size) != 0) return -1;
    memcpy(out, b, 4);
    return 0;
}

static void cmd_temps(void) {
    char b[32]; uint32_t size, type;
    for (size_t i = 0; i < sizeof(TEMP_KEYS)/sizeof(TEMP_KEYS[0]); i++) {
        if (smc_keyinfo(TEMP_KEYS[i], &size, &type) != 0 || size == 0) continue;
        if (type != t_flt || size != 4) continue;
        if (smc_read(TEMP_KEYS[i], b, &size) != 0) continue;
        float f; memcpy(&f, b, 4);
        printf("temp %s %.1f\n", TEMP_KEYS[i], f);
    }
}

static void cmd_temp(void) {
    /* 用稳定温度源的中位数作为控制温度（TCMz 瞬时热点抖动 ±17°C 不可用） */
    float vals[16];
    int n = 0;
    for (size_t i = 0; i < sizeof(STABLE_TEMP_KEYS)/sizeof(STABLE_TEMP_KEYS[0]) && n < 16; i++) {
        float f;
        if (read_float_key(STABLE_TEMP_KEYS[i], &f) == 0 && f > 0 && f < 150)
            vals[n++] = f;
    }
    /* 简单插入排序取中位数 */
    for (int i = 1; i < n; i++) {
        float key = vals[i];
        int j = i - 1;
        while (j >= 0 && vals[j] > key) { vals[j+1] = vals[j]; j--; }
        vals[j+1] = key;
    }
    if (n == 0) { printf("temp - 0\n"); return; }
    float med = vals[n/2];
    /* 中位数代表整体温度；同时输出最高稳定值供参考 */
    float hi = vals[n-1];
    printf("temp %s %.1f\n", "median", med);
    printf("temp2 %s %.1f\n", "max", hi);
}

static void cmd_get(const char *key) {
    if (strlen(key) != 4) { fprintf(stderr, "[ERROR] key must be 4 chars\n"); exit(1); }
    uint32_t size, type;
    if (smc_keyinfo(key, &size, &type) != 0 || size == 0) {
        fprintf(stderr, "[ERROR] key %s unavailable\n", key); exit(1);
    }
    char b[32];
    if (smc_read(key, b, &size) != 0) {
        fprintf(stderr, "[ERROR] read %s failed\n", key); exit(1);
    }
    char t4[5]; deccfour(type, t4);
    char val[128];
    decode(key, b, size, type, val, sizeof(val));
    printf("%s %s %u %s\n", key, t4, size, val);
}

static void cmd_set(const char *key, const char *val) {
    if (strlen(key) != 4) { fprintf(stderr, "[ERROR] key must be 4 chars\n"); exit(1); }
    uint32_t size, type;
    if (smc_keyinfo(key, &size, &type) != 0 || size == 0) {
        fprintf(stderr, "[ERROR] key %s unavailable\n", key); exit(1);
    }
    char b[32];
    if (encode(val, b, size, type) != 0) {
        fprintf(stderr, "[ERROR] cannot encode value for type\n"); exit(1);
    }
    if (smc_write(key, b, size, type) != 0) {
        fprintf(stderr, "[ERROR] write %s failed (need root?)\n", key); exit(1);
    }
    printf("ok %s %s\n", key, val);
}

static void cmd_fan(int mode, int have_mode, int speed, int have_speed) {
    char b[32];
    uint32_t size, type;
    if (smc_keyinfo("F0Md", &size, &type) != 0 || size == 0) {
        fprintf(stderr, "[ERROR] F0Md unavailable\n"); exit(1);
    }
    if (have_mode) {
        b[0] = (char)mode;
        if (smc_write("F0Md", b, size, type) != 0) {
            fprintf(stderr, "[ERROR] set mode failed\n"); exit(1);
        }
        printf("mode %d\n", mode);
    }
    if (have_speed) {
        if (smc_keyinfo("F0Tg", &size, &type) != 0 || size == 0) {
            fprintf(stderr, "[ERROR] F0Tg unavailable\n"); exit(1);
        }
        float f = (float)speed;
        memcpy(b, &f, 4);
        if (smc_write("F0Tg", b, size, type) != 0) {
            fprintf(stderr, "[ERROR] set speed failed\n"); exit(1);
        }
        printf("speed %d\n", speed);
    }
}

static void cmd_reset(void) {
    char b[32] = {0};
    uint32_t size, type;
    if (smc_keyinfo("F0Md", &size, &type) != 0 || size == 0) {
        fprintf(stderr, "[ERROR] F0Md unavailable\n"); exit(1);
    }
    b[0] = 0;
    if (smc_write("F0Md", b, size, type) != 0) {
        fprintf(stderr, "[ERROR] reset FAILED\n"); exit(1);
    }
    printf("reset ok\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: fanctl-smc fans|temps|temp|get|set|fan|reset\n");
        return 1;
    }
    if (smc_open() != 0) return 1;
    const char *cmd = argv[1];
    if (strcmp(cmd, "fans") == 0) cmd_fans();
    else if (strcmp(cmd, "temps") == 0) cmd_temps();
    else if (strcmp(cmd, "temp") == 0) cmd_temp();
    else if (strcmp(cmd, "get") == 0 && argc >= 3) cmd_get(argv[2]);
    else if (strcmp(cmd, "set") == 0 && argc >= 4) cmd_set(argv[2], argv[3]);
    else if (strcmp(cmd, "fan") == 0) {
        int mode = 0, have_mode = 0, speed = 0, have_speed = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
                mode = atoi(argv[++i]); have_mode = 1;
            } else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
                speed = atoi(argv[++i]); have_speed = 1;
            }
        }
        cmd_fan(mode, have_mode, speed, have_speed);
    }
    else if (strcmp(cmd, "reset") == 0) cmd_reset();
    else { fprintf(stderr, "unknown command\n"); return 1; }
    return 0;
}
