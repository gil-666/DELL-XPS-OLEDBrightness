/*
 * oled_brightness.c — CLI tool for OLEDBrightness kext
 *
 * Usage:
 *   oled_brightness get           # Print current brightness (0-65535)
 *   oled_brightness set <value>   # Set brightness (0-65535 or 0%-100%)
 *   oled_brightness max           # Print max brightness value
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <IOKit/IOKitLib.h>

enum {
    kMethodGetBrightness = 0,
    kMethodSetBrightness = 1,
    kMethodGetMax        = 2,
};

static io_connect_t openUserClient(void) {
    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault,
        IOServiceMatching("OLEDBrightnessDriver"));

    if (!service) {
        fprintf(stderr, "Error: OLEDBrightnessDriver not found.\n");
        fprintf(stderr, "Is the kext loaded? Check: ioreg -rn OLEDBrightnessDriver\n");
        return IO_OBJECT_NULL;
    }

    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    IOObjectRelease(service);

    if (kr != KERN_SUCCESS || conn == IO_OBJECT_NULL) {
        fprintf(stderr, "Error: Failed to open user client (0x%08x).\n", kr);
        fprintf(stderr, "Try running as root: sudo oled_brightness ...\n");
        return IO_OBJECT_NULL;
    }

    return conn;
}

static int cmd_get(io_connect_t conn) {
    uint64_t output = 0;
    uint32_t outputCount = 1;

    kern_return_t kr = IOConnectCallScalarMethod(
        conn, kMethodGetBrightness, NULL, 0, &output, &outputCount);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Error: getBrightness failed (0x%08x)\n", kr);
        return 1;
    }

    uint16_t level = (uint16_t)(output & 0xFFFF);
    double pct = (double)level / 65535.0 * 100.0;
    double nits = pct / 100.0 * 440.0;
    printf("%u (%.1f%%, ~%.0f nits)\n", level, pct, nits);
    return 0;
}

static int cmd_set(io_connect_t conn, const char *value_str) {
    uint16_t level;
    size_t len = strlen(value_str);

    if (len > 0 && value_str[len - 1] == '%') {
        char buf[32];
        strncpy(buf, value_str, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        buf[len - 1] = '\0';
        double pct = atof(buf);
        if (pct < 0.0) pct = 0.0;
        if (pct > 100.0) pct = 100.0;
        level = (uint16_t)(pct / 100.0 * 65535.0);
    } else {
        long val = atol(value_str);
        if (val < 0) val = 0;
        if (val > 65535) val = 65535;
        level = (uint16_t)val;
    }

    uint64_t input = level;
    kern_return_t kr = IOConnectCallScalarMethod(
        conn, kMethodSetBrightness, &input, 1, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Error: setBrightness(%u) failed (0x%08x)\n", level, kr);
        return 1;
    }

    double pct = (double)level / 65535.0 * 100.0;
    double nits = pct / 100.0 * 440.0;
    printf("Brightness set to %u (%.1f%%, ~%.0f nits)\n", level, pct, nits);
    return 0;
}

static int cmd_max(io_connect_t conn) {
    uint64_t output = 0;
    uint32_t outputCount = 1;

    kern_return_t kr = IOConnectCallScalarMethod(
        conn, kMethodGetMax, NULL, 0, &output, &outputCount);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Error: getMax failed (0x%08x)\n", kr);
        return 1;
    }

    printf("%llu\n", output);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s get              Get current brightness\n", prog);
    fprintf(stderr, "  %s set <value>      Set brightness (0-65535 or 0%%-100%%)\n", prog);
    fprintf(stderr, "  %s max              Get max brightness value\n", prog);
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s set 50%%          Set to 50%%\n", prog);
    fprintf(stderr, "  %s set 32768        Set to ~50%% (raw value)\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    io_connect_t conn = openUserClient();
    if (conn == IO_OBJECT_NULL) return 1;

    int ret;
    if (strcmp(argv[1], "get") == 0) {
        ret = cmd_get(conn);
    } else if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: 'set' requires a value argument.\n");
            ret = 1;
        } else {
            ret = cmd_set(conn, argv[2]);
        }
    } else if (strcmp(argv[1], "max") == 0) {
        ret = cmd_max(conn);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        usage(argv[0]);
        ret = 1;
    }

    IOServiceClose(conn);
    return ret;
}
