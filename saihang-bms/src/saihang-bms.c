#define _GNU_SOURCE
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define APP_VERSION "2.2.0"
#define DEFAULT_TIMEOUT_MS 4000
#define MAX_FRAME 1024
#define MAX_DEVICES 64
#define ATT_CID 4
#define ATT_OP_MTU_REQ 0x02
#define ATT_OP_MTU_RSP 0x03
#define ATT_OP_FIND_BY_TYPE_REQ 0x06
#define ATT_OP_FIND_BY_TYPE_RSP 0x07
#define ATT_OP_READ_BY_TYPE_REQ 0x08
#define ATT_OP_READ_BY_TYPE_RSP 0x09
#define ATT_OP_READ_REQ 0x0a
#define ATT_OP_READ_RSP 0x0b
#define ATT_OP_READ_BLOB_REQ 0x0c
#define ATT_OP_READ_BLOB_RSP 0x0d
#define ATT_OP_WRITE_REQ 0x12
#define ATT_OP_WRITE_RSP 0x13
#define ATT_OP_HANDLE_NOTIFY 0x1b
#define ATT_OP_ERROR 0x01

#define MGMT_OP_READ_INDEX_LIST 0x0003
#define MGMT_OP_READ_INFO 0x0004
#define MGMT_OP_START_DISCOVERY 0x0023
#define MGMT_OP_STOP_DISCOVERY 0x0024
#define MGMT_EV_CMD_COMPLETE 0x0001
#define MGMT_EV_DEVICE_FOUND 0x0012
#define MGMT_INDEX_NONE 0xffff
#define MGMT_DISCOV_LE 0x06

static bool g_pretty = false;
static bool g_include_raw = false;
static bool g_verbose = false;
static int g_adapter = -1;
static int g_timeout_ms = DEFAULT_TIMEOUT_MS;
static bool g_recover = true;
static int g_recover_retries = 2;
static int g_recover_delay_ms = 1500;

static void dbgf(const char *fmt, ...)
{
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void msleep_int(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static uint16_t le16_get(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t be32_get(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int32_t be32s_get(const uint8_t *p)
{
    return (int32_t)be32_get(p);
}

static uint16_t be16_get(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint16_t crc16_modbus(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : (crc >> 1);
    }
    return crc;
}

static void json_string(const char *s)
{
    putchar('"');
    if (s) {
        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            switch (c) {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (c < 0x20) printf("\\u%04x", c);
                else putchar(c);
            }
        }
    }
    putchar('"');
}

static void nl(void) { if (g_pretty) putchar('\n'); }
static void indent(int n) { if (g_pretty) while (n--) putchar(' '); }
static void sep(void) { putchar(','); nl(); }
static void key(int ind, const char *k) { indent(ind); json_string(k); fputs(g_pretty ? ": " : ":", stdout); }

static int poll_read(int fd, void *buf, size_t len, int timeout_ms)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int r;
    do r = poll(&pfd, 1, timeout_ms); while (r < 0 && errno == EINTR);
    if (r == 0) return -ETIMEDOUT;
    if (r < 0) return -errno;
    ssize_t n = read(fd, buf, len);
    if (n < 0) return -errno;
    return (int)n;
}

struct mgmt_hdr { uint16_t opcode, index, len; } __attribute__((packed));

static int mgmt_open(void)
{
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) return -errno;
    struct sockaddr_hci a = {0};
    a.hci_family = AF_BLUETOOTH;
    a.hci_dev = HCI_DEV_NONE;
    a.hci_channel = HCI_CHANNEL_CONTROL;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        int e = -errno; close(fd); return e;
    }
    return fd;
}

static int mgmt_send(int fd, uint16_t op, uint16_t index, const void *data, uint16_t len)
{
    uint8_t buf[512];
    if (sizeof(struct mgmt_hdr) + len > sizeof(buf)) return -EMSGSIZE;
    struct mgmt_hdr *h = (struct mgmt_hdr *)buf;
    h->opcode = htole16(op);
    h->index = htole16(index);
    h->len = htole16(len);
    if (len) memcpy(buf + sizeof(*h), data, len);
    ssize_t n = write(fd, buf, sizeof(*h) + len);
    return n < 0 ? -errno : 0;
}

static const char *mgmt_status_name(uint8_t s)
{
    switch (s) {
    case 0x00: return "Success";
    case 0x01: return "Unknown Command";
    case 0x02: return "Not Connected";
    case 0x03: return "Failed";
    case 0x04: return "Connect Failed";
    case 0x05: return "Authentication Failed";
    case 0x06: return "Not Paired";
    case 0x07: return "No Resources";
    case 0x08: return "Timeout";
    case 0x09: return "Already Connected";
    case 0x0a: return "Busy";
    case 0x0b: return "Rejected";
    case 0x0c: return "Not Supported";
    case 0x0d: return "Invalid Parameters";
    case 0x0e: return "Disconnected";
    case 0x0f: return "Not Powered";
    case 0x10: return "Cancelled";
    case 0x11: return "Invalid Index";
    case 0x12: return "RFKILLED";
    case 0x13: return "Already Paired";
    case 0x14: return "Permission Denied";
    default: return "Unknown";
    }
}

static int mgmt_read_index_list(uint16_t *out)
{
    int fd = mgmt_open();
    if (fd < 0) return fd;
    int r = mgmt_send(fd, MGMT_OP_READ_INDEX_LIST, MGMT_INDEX_NONE, NULL, 0);
    if (r < 0) { close(fd); return r; }
    uint8_t buf[1024];
    for (;;) {
        int n = poll_read(fd, buf, sizeof(buf), 2000);
        if (n < 0) { close(fd); return n; }
        if ((size_t)n < sizeof(struct mgmt_hdr) + 3) continue;
        struct mgmt_hdr *h = (struct mgmt_hdr *)buf;
        if (le16toh(h->opcode) != MGMT_EV_CMD_COMPLETE) continue;
        uint8_t *p = buf + sizeof(*h);
        if (le16_get(p) != MGMT_OP_READ_INDEX_LIST) continue;
        uint8_t status = p[2];
        if (status) { close(fd); return status == 0x11 ? -ENODEV : -EIO; }
        if ((size_t)n < sizeof(*h) + 5) { close(fd); return -EPROTO; }
        uint16_t count = le16_get(p + 3);
        if (!count) { close(fd); return -ENODEV; }
        *out = le16_get(p + 5);
        close(fd);
        return 0;
    }
}

static int resolve_current_index(uint16_t *idx)
{
    if (g_adapter >= 0) { *idx = (uint16_t)g_adapter; return 0; }
    return mgmt_read_index_list(idx);
}

struct discovered {
    char addr[18];
    uint8_t addr_type;
    int8_t rssi;
    char name[80];
    bool has_fffa;
};

static void parse_eir(struct discovered *d, const uint8_t *eir, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t l = eir[i++];
        if (!l || i + l > len + 1) break;
        uint8_t t = eir[i++];
        size_t dl = l - 1;
        if ((t == 0x08 || t == 0x09) && dl) {
            size_t n = dl < sizeof(d->name) - 1 ? dl : sizeof(d->name) - 1;
            memcpy(d->name, eir + i, n); d->name[n] = 0;
        } else if ((t == 0x02 || t == 0x03) && dl >= 2) {
            for (size_t j = 0; j + 1 < dl; j += 2)
                if (le16_get(eir + i + j) == 0xfffa) d->has_fffa = true;
        }
        i += dl;
    }
}

static int find_or_add(struct discovered *devs, int *count, const char *addr, uint8_t at)
{
    for (int i = 0; i < *count; i++) if (!strcmp(devs[i].addr, addr)) return i;
    if (*count >= MAX_DEVICES) return -1;
    int i = (*count)++;
    memset(&devs[i], 0, sizeof(devs[i]));
    snprintf(devs[i].addr, sizeof(devs[i].addr), "%s", addr);
    devs[i].addr_type = at;
    devs[i].rssi = -127;
    return i;
}

static int mgmt_stop_discovery_now(void)
{
    uint16_t idx;
    int r = resolve_current_index(&idx); if (r < 0) return r;
    int fd = mgmt_open(); if (fd < 0) return fd;
    uint8_t type = MGMT_DISCOV_LE;
    r = mgmt_send(fd, MGMT_OP_STOP_DISCOVERY, idx, &type, 1);
    close(fd);
    return r;
}

static int scan_devices(int timeout_sec, bool all, struct discovered *devs, int *count)
{
    *count = 0;
    uint16_t idx;
    int r = resolve_current_index(&idx);
    if (r < 0) return r;
    dbgf("using Bluetooth controller index %u\n", idx);
    int fd = mgmt_open(); if (fd < 0) return fd;
    uint8_t type = MGMT_DISCOV_LE;
    r = mgmt_send(fd, MGMT_OP_START_DISCOVERY, idx, &type, 1);
    if (r < 0) { close(fd); return r; }
    bool started = false;
    time_t end = time(NULL) + timeout_sec;
    uint8_t buf[2048];
    while (time(NULL) < end) {
        int n = poll_read(fd, buf, sizeof(buf), 500);
        if (n == -ETIMEDOUT) continue;
        if (n < 0) { close(fd); return n; }
        if ((size_t)n < sizeof(struct mgmt_hdr)) continue;
        struct mgmt_hdr *h = (struct mgmt_hdr *)buf;
        uint16_t ev = le16toh(h->opcode);
        uint8_t *p = buf + sizeof(*h);
        if (ev == MGMT_EV_CMD_COMPLETE && n >= (int)sizeof(*h) + 3) {
            uint16_t op = le16_get(p); uint8_t st = p[2];
            if (op == MGMT_OP_START_DISCOVERY) {
                if (st == 0) started = true;
                else {
                    fprintf(stderr, "cannot start discovery: mgmt status 0x%02x (%s)\n", st, mgmt_status_name(st));
                    close(fd);
                    return st == 0x0a ? -EBUSY : st == 0x11 ? -ENODEV : -EIO;
                }
            }
        } else if (ev == MGMT_EV_DEVICE_FOUND && n >= (int)sizeof(*h) + 14) {
            bdaddr_t ba; memcpy(&ba, p, 6);
            char addr[18]; ba2str(&ba, addr);
            uint8_t at = p[6]; int8_t rssi = (int8_t)p[7];
            uint32_t flags; memcpy(&flags, p + 8, 4); (void)flags;
            uint16_t eir_len = le16_get(p + 12);
            if ((size_t)n < sizeof(*h) + 14 + eir_len) continue;
            int i = find_or_add(devs, count, addr, at);
            if (i >= 0) { devs[i].rssi = rssi; parse_eir(&devs[i], p + 14, eir_len); }
        }
    }
    if (started) (void)mgmt_send(fd, MGMT_OP_STOP_DISCOVERY, idx, &type, 1);
    close(fd);
    if (!all) {
        int w = 0;
        for (int i = 0; i < *count; i++)
            if (devs[i].has_fffa && !strncmp(devs[i].name, "SH", 2)) devs[w++] = devs[i];
        *count = w;
    }
    return 0;
}

struct adapter_sysfs {
    char device_path[PATH_MAX];
    char usb_interface[64];
    char usb_device[64];
    char vendor[16];
    char product[16];
};

static int read_small_file(const char *p, char *out, size_t cap)
{
    int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) return -errno;
    ssize_t n = read(fd, out, cap - 1); close(fd); if (n < 0) return -errno;
    out[n] = 0; while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = 0;
    return 0;
}

static int adapter_sysfs_info(uint16_t idx, struct adapter_sysfs *a)
{
    memset(a, 0, sizeof(*a));
    char p[PATH_MAX], rp[PATH_MAX];
    snprintf(p, sizeof(p), "/sys/class/bluetooth/hci%u/device", idx);
    if (!realpath(p, rp)) return -errno;
    snprintf(a->device_path, sizeof(a->device_path), "%s", rp);
    const char *b = strrchr(rp, '/'); if (!b) return -EINVAL;
    snprintf(a->usb_interface, sizeof(a->usb_interface), "%s", b + 1);
    char tmp[PATH_MAX]; snprintf(tmp, sizeof(tmp), "%s", rp);
    char *slash = strrchr(tmp, '/'); if (!slash) return -EINVAL; *slash = 0;
    b = strrchr(tmp, '/'); if (!b) return -EINVAL;
    snprintf(a->usb_device, sizeof(a->usb_device), "%s", b + 1);
    snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/idVendor", a->usb_device); (void)read_small_file(p, a->vendor, sizeof(a->vendor));
    snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/idProduct", a->usb_device); (void)read_small_file(p, a->product, sizeof(a->product));
    return 0;
}

static int write_text(const char *p, const char *s)
{
    int fd = open(p, O_WRONLY | O_CLOEXEC); if (fd < 0) return -errno;
    ssize_t n = write(fd, s, strlen(s)); int e = n < 0 ? -errno : 0; close(fd); return e;
}

static int wait_hci(uint16_t idx, int ms)
{
    char p[128]; snprintf(p, sizeof(p), "/sys/class/bluetooth/hci%u", idx);
    for (int t = 0; t < ms; t += 100) { if (access(p, F_OK) == 0) return 0; msleep_int(100); }
    return -ETIMEDOUT;
}

static int adapter_reset_level(bool hard)
{
    uint16_t idx; int r = resolve_current_index(&idx); if (r < 0) return r;
    struct adapter_sysfs a; r = adapter_sysfs_info(idx, &a); if (r < 0) return r;
    if (!hard) {
        dbgf("recovery: soft reset hci%u via btusb unbind/bind (%s)\n", idx, a.usb_interface);
        r = write_text("/sys/bus/usb/drivers/btusb/unbind", a.usb_interface); if (r < 0) return r;
        msleep_int(500);
        r = write_text("/sys/bus/usb/drivers/btusb/bind", a.usb_interface); if (r < 0) return r;
    } else {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/authorized", a.usb_device);
        dbgf("recovery: hard USB reauthorize %s (%s:%s)\n", a.usb_device, a.vendor, a.product);
        r = write_text(p, "0"); if (r < 0) return r;
        msleep_int(700);
        r = write_text(p, "1"); if (r < 0) return r;
    }
    msleep_int(g_recover_delay_ms);
    return wait_hci(idx, 5000);
}

struct att_conn {
    int fd;
    uint16_t mtu;
    uint16_t svc_start, svc_end, tx, rx, cccd;
    int addr_type;
};

static int att_connect_once(const char *mac, int addr_type, struct att_conn *ac)
{
    memset(ac, 0, sizeof(*ac)); ac->fd = -1; ac->mtu = 23; ac->addr_type = addr_type;
    bdaddr_t dst; if (str2ba(mac, &dst) < 0) return -EINVAL;
    int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP); if (fd < 0) return -errno;
    struct bt_security sec = {.level = BT_SECURITY_LOW}; setsockopt(fd, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec));
    struct sockaddr_l2 src = {0}, dstaddr = {0};
    src.l2_family = AF_BLUETOOTH; bacpy(&src.l2_bdaddr, BDADDR_ANY); src.l2_cid = htobs(ATT_CID); src.l2_bdaddr_type = addr_type;
    if (bind(fd, (struct sockaddr *)&src, sizeof(src)) < 0) { int e = -errno; close(fd); return e; }
    dstaddr.l2_family = AF_BLUETOOTH; bacpy(&dstaddr.l2_bdaddr, &dst); dstaddr.l2_cid = htobs(ATT_CID); dstaddr.l2_bdaddr_type = addr_type;
    if (connect(fd, (struct sockaddr *)&dstaddr, sizeof(dstaddr)) < 0) { int e = -errno; close(fd); return e; }
    ac->fd = fd;
    uint8_t req[3] = {ATT_OP_MTU_REQ, 0x05, 0x02};
    if (write(fd, req, sizeof(req)) == sizeof(req)) {
        uint8_t rsp[64]; int n = poll_read(fd, rsp, sizeof(rsp), g_timeout_ms);
        if (n >= 3 && rsp[0] == ATT_OP_MTU_RSP) ac->mtu = le16_get(rsp + 1);
    }
    return 0;
}

static int att_connect_auto(const char *mac, int pref, struct att_conn *ac)
{
    if (pref == BDADDR_LE_PUBLIC || pref == BDADDR_LE_RANDOM) return att_connect_once(mac, pref, ac);
    int r = att_connect_once(mac, BDADDR_LE_PUBLIC, ac);
    if (r < 0) r = att_connect_once(mac, BDADDR_LE_RANDOM, ac);
    return r;
}

static int att_xact(struct att_conn *ac, const uint8_t *req, size_t qlen, uint8_t *rsp, size_t cap, uint8_t want)
{
    if (write(ac->fd, req, qlen) != (ssize_t)qlen) return -errno;
    for (;;) {
        int n = poll_read(ac->fd, rsp, cap, g_timeout_ms); if (n < 0) return n;
        if (n > 0 && rsp[0] == want) return n;
        if (n >= 5 && rsp[0] == ATT_OP_ERROR) return -EIO;
    }
}

static int discover_primary_fffa(struct att_conn *ac)
{
    uint8_t q[] = {ATT_OP_FIND_BY_TYPE_REQ, 0x01,0x00, 0xff,0xff, 0x00,0x28, 0xfa,0xff};
    uint8_t r[256]; int n = att_xact(ac, q, sizeof(q), r, sizeof(r), ATT_OP_FIND_BY_TYPE_RSP); if (n < 5) return -ENOENT;
    ac->svc_start = le16_get(r + 1); ac->svc_end = le16_get(r + 3); return 0;
}

static int discover_chars(struct att_conn *ac)
{
    uint16_t s = ac->svc_start, e = ac->svc_end;
    while (s <= e) {
        uint8_t q[] = {ATT_OP_READ_BY_TYPE_REQ, s&0xff,s>>8, e&0xff,e>>8, 0x03,0x28};
        uint8_t r[512]; int n = att_xact(ac, q, sizeof(q), r, sizeof(r), ATT_OP_READ_BY_TYPE_RSP);
        if (n < 2) break;
        int es = r[1]; if (es < 7) break;
        uint16_t last = s;
        for (int off = 2; off + es <= n; off += es) {
            uint16_t decl = le16_get(r + off); uint16_t val = le16_get(r + off + 3); uint16_t uuid = le16_get(r + off + 5);
            dbgf("char %04x value=0x%04x props=0x%02x uuid=%04x\n", decl, val, r[off+2], uuid);
            if (uuid == 0xfffb) ac->tx = val;
            if (uuid == 0xfffc) ac->rx = val;
            last = decl;
        }
        if (last == 0xffff || last + 1 > e) break; s = last + 1;
    }
    if (!ac->tx || !ac->rx) return -ENOENT;
    ac->cccd = ac->rx + 1;
    return 0;
}

static int att_write_handle(struct att_conn *ac, uint16_t h, const uint8_t *data, size_t len)
{
    uint8_t q[512]; if (len + 3 > sizeof(q)) return -EMSGSIZE;
    q[0] = ATT_OP_WRITE_REQ; q[1] = h & 0xff; q[2] = h >> 8; memcpy(q + 3, data, len);
    uint8_t r[64]; int n = att_xact(ac, q, len + 3, r, sizeof(r), ATT_OP_WRITE_RSP); return n < 0 ? n : 0;
}

static int setup_saihang(struct att_conn *ac)
{
    int r = discover_primary_fffa(ac); if (r < 0) return r;
    dbgf("FFFA service: 0x%04x-0x%04x\n", ac->svc_start, ac->svc_end);
    r = discover_chars(ac); if (r < 0) return r;
    uint8_t en[] = {1,0};
    return att_write_handle(ac, ac->cccd, en, sizeof(en));
}

static int send_modbus_read(struct att_conn *ac, uint8_t fn, uint16_t start, uint16_t count, uint8_t *frame, size_t cap, size_t *flen)
{
    uint8_t cmd[10] = {0xa5,0xa5,0x00,fn,(uint8_t)(start>>8),(uint8_t)start,(uint8_t)(count>>8),(uint8_t)count,0,0};
    uint16_t crc = crc16_modbus(cmd + 2, 6); cmd[8] = crc & 0xff; cmd[9] = crc >> 8;
    int r = att_write_handle(ac, ac->tx, cmd, sizeof(cmd)); if (r < 0) return r;
    size_t got = 0; time_t deadline = time(NULL) + 5;
    while (time(NULL) <= deadline && got < cap) {
        uint8_t b[1024]; int n = poll_read(ac->fd, b, sizeof(b), g_timeout_ms); if (n < 0) return n;
        if (n >= 3 && b[0] == ATT_OP_HANDLE_NOTIFY && le16_get(b+1) == ac->rx) {
            size_t x = n - 3; if (got + x > cap) x = cap - got; memcpy(frame + got, b + 3, x); got += x;
            if (got >= 5) { size_t need = 7 + frame[4]; if (got >= need) { *flen = need; return 0; } }
        }
    }
    return -ETIMEDOUT;
}

struct sample {
    char address[18]; char name[80]; int addr_type; uint16_t mtu;
    double voltage, current; int soc, soh; double remaining_ah, design_ah; int cycles;
    uint16_t problem, mosfet, balancer; bool charge_mos, discharge_mos;
    int cell_count; double cells[20]; int temp_count; double temps[10]; double mosfet_t, ambient_t;
    uint16_t svc_start, svc_end, tx, rx, cccd; size_t frame_len; bool crc_ok; uint8_t raw[MAX_FRAME];
};

static int decode_sample(const char *mac, const char *name, struct att_conn *ac, uint8_t *f, size_t n, struct sample *s)
{
    if (n < 111 || f[0] != 0xa5 || f[1] != 0xa5) return -EPROTO;
    size_t need = 7 + f[4]; if (n < need) return -EPROTO;
    uint16_t got = (uint16_t)f[need-2] | ((uint16_t)f[need-1] << 8); uint16_t calc = crc16_modbus(f + 2, need - 4);
    memset(s,0,sizeof(*s)); snprintf(s->address,sizeof(s->address),"%s",mac); snprintf(s->name,sizeof(s->name),"%s",name?name:"");
    s->addr_type = ac->addr_type; s->mtu=ac->mtu; s->svc_start=ac->svc_start; s->svc_end=ac->svc_end; s->tx=ac->tx; s->rx=ac->rx; s->cccd=ac->cccd;
    s->frame_len=need; s->crc_ok = got == calc; memcpy(s->raw,f,need);
    s->current = be32s_get(f+5)/100.0; s->voltage = be32_get(f+9)/100.0; s->soc=be16_get(f+13); s->soh=be16_get(f+15);
    s->remaining_ah=be32_get(f+17)/100.0; s->design_ah=be32_get(f+25)/100.0; s->cycles=be16_get(f+29);
    s->problem=(uint16_t)(~be16_get(f+31)); s->mosfet=be16_get(f+37); s->charge_mos=(s->mosfet&0x0200)!=0; s->discharge_mos=(s->mosfet&0x0400)!=0; s->balancer=be16_get(f+41);
    s->cell_count=be16_get(f+43); if(s->cell_count>20)s->cell_count=20; for(int i=0;i<s->cell_count;i++)s->cells[i]=be16_get(f+45+i*2)/1000.0;
    s->temp_count=be16_get(f+85); if(s->temp_count>10)s->temp_count=10; for(int i=0;i<s->temp_count;i++)s->temps[i]=(be16_get(f+87+i*2)-2730)/10.0;
    s->mosfet_t=(be16_get(f+107)-2730)/10.0; s->ambient_t=(be16_get(f+109)-2730)/10.0;
    return 0;
}

static void print_hex_string(const uint8_t *p, size_t n) { putchar('"'); for(size_t i=0;i<n;i++)printf("%02x",p[i]); putchar('"'); }

static void print_sample_json(const struct sample *s)
{
    double power=s->voltage*s->current, rem_wh=s->voltage*s->remaining_ah, design_wh=s->voltage*s->design_ah;
    double cmin=0,cmax=0,sum=0,avg=0,stdmv=0,maxdev=0; int mini=0,maxi=0;
    if(s->cell_count){cmin=cmax=s->cells[0];for(int i=0;i<s->cell_count;i++){double v=s->cells[i];sum+=v;if(v<cmin){cmin=v;mini=i;}if(v>cmax){cmax=v;maxi=i;}}avg=sum/s->cell_count;double ss=0;for(int i=0;i<s->cell_count;i++){double d=(s->cells[i]-avg)*1000;ss+=d*d;if(fabs(d)>maxdev)maxdev=fabs(d);}stdmv=sqrt(ss/s->cell_count);}
    double tmin=0,tmax=0,tsum=0; if(s->temp_count){tmin=tmax=s->temps[0];for(int i=0;i<s->temp_count;i++){if(s->temps[i]<tmin)tmin=s->temps[i];if(s->temps[i]>tmax)tmax=s->temps[i];tsum+=s->temps[i];}}
    double omin=s->mosfet_t,omax=s->mosfet_t,osum=s->mosfet_t+s->ambient_t;int oc=2;if(s->ambient_t<omin)omin=s->ambient_t;if(s->ambient_t>omax)omax=s->ambient_t;for(int i=0;i<s->temp_count;i++){if(s->temps[i]<omin)omin=s->temps[i];if(s->temps[i]>omax)omax=s->temps[i];osum+=s->temps[i];oc++;}
    putchar('{'); nl(); key(2,"schema"); json_string("saihang-bms/v2"); sep(); key(2,"timestamp_unix"); printf("%ld",(long)time(NULL)); sep();
    key(2,"device"); putchar('{'); nl(); key(4,"address");json_string(s->address);sep();key(4,"address_type");json_string(s->addr_type==BDADDR_LE_RANDOM?"random":"public"); if(s->name[0]){sep();key(4,"name");json_string(s->name);} nl();indent(2);putchar('}');sep();
    key(2,"pack");putchar('{');nl();key(4,"voltage_v");printf("%.2f",s->voltage);sep();key(4,"current_a");printf("%.2f",s->current);sep();key(4,"power_w");printf("%.2f",power);sep();key(4,"soc_pct");printf("%d",s->soc);sep();key(4,"soh_pct");printf("%d",s->soh);sep();key(4,"charging");fputs(s->current>0?"true":"false",stdout);if(s->current<0){sep();key(4,"runtime_s_est");printf("%.0f",s->remaining_ah/fabs(s->current)*3600);}nl();indent(2);putchar('}');sep();
    key(2,"capacity");putchar('{');nl();key(4,"remaining_ah");printf("%.2f",s->remaining_ah);sep();key(4,"design_ah");printf("%.2f",s->design_ah);sep();key(4,"remaining_wh_est");printf("%.2f",rem_wh);sep();key(4,"design_wh_at_present_voltage");printf("%.2f",design_wh);sep();key(4,"soc_from_capacity_pct");printf("%.2f",s->design_ah>0?s->remaining_ah/s->design_ah*100:0);sep();key(4,"soc_capacity_error_pct_points");printf("%.2f",s->design_ah>0?s->soc-s->remaining_ah/s->design_ah*100:0);sep();key(4,"cycles");printf("%d",s->cycles);nl();indent(2);putchar('}');sep();
    key(2,"status");putchar('{');nl();key(4,"problem");fputs(s->problem?"true":"false",stdout);sep();key(4,"problem_code");printf("%u",s->problem);sep();key(4,"problem_code_hex");{char x[16];snprintf(x,sizeof(x),"0x%04x",s->problem);json_string(x);}sep();key(4,"problem_bits_set");putchar('[');bool first=true;for(int i=0;i<16;i++)if(s->problem&(1u<<i)){if(!first)putchar(',');printf("%d",i);first=false;}putchar(']');sep();key(4,"charge_mosfet");fputs(s->charge_mos?"true":"false",stdout);sep();key(4,"discharge_mosfet");fputs(s->discharge_mos?"true":"false",stdout);sep();key(4,"mosfet_raw");printf("%u",s->mosfet);sep();key(4,"balancer_raw");printf("%u",s->balancer);sep();key(4,"balancing_active");fputs(s->balancer?"true":"false",stdout);nl();indent(2);putchar('}');sep();
    key(2,"cells");putchar('{');nl();key(4,"count");printf("%d",s->cell_count);sep();key(4,"voltages_v");putchar('[');for(int i=0;i<s->cell_count;i++){if(i)putchar(',');if(g_pretty)putchar(' ');printf("%.3f",s->cells[i]);}putchar(']');sep();key(4,"sum_v");printf("%.3f",sum);sep();key(4,"average_v");printf("%.3f",avg);sep();key(4,"min_v");printf("%.3f",cmin);sep();key(4,"min_cell_index");printf("%d",s->cell_count?mini+1:0);sep();key(4,"max_v");printf("%.3f",cmax);sep();key(4,"max_cell_index");printf("%d",s->cell_count?maxi+1:0);sep();key(4,"delta_v");printf("%.3f",cmax-cmin);sep();key(4,"delta_mv");printf("%.1f",(cmax-cmin)*1000);sep();key(4,"spread_pct_of_average");printf("%.3f",avg?(cmax-cmin)/avg*100:0);sep();key(4,"stddev_mv");printf("%.2f",stdmv);sep();key(4,"max_deviation_from_average_mv");printf("%.2f",maxdev);sep();key(4,"pack_minus_cell_sum_v");printf("%.3f",s->voltage-sum);sep();key(4,"detail");putchar('[');for(int i=0;i<s->cell_count;i++){if(i)putchar(',');if(g_pretty)putchar(' ');printf("{\"index\":%d,\"voltage_v\":%.3f,\"deviation_mv\":%.1f}",i+1,s->cells[i],(s->cells[i]-avg)*1000);}putchar(']');nl();indent(2);putchar('}');sep();
    key(2,"temperatures");putchar('{');nl();key(4,"cell_sensor_count");printf("%d",s->temp_count);sep();key(4,"cell_c");putchar('[');for(int i=0;i<s->temp_count;i++){if(i)putchar(',');if(g_pretty)putchar(' ');printf("%.1f",s->temps[i]);}putchar(']');sep();key(4,"cell_min_c");printf("%.1f",tmin);sep();key(4,"cell_max_c");printf("%.1f",tmax);sep();key(4,"cell_average_c");printf("%.1f",s->temp_count?tsum/s->temp_count:0);sep();key(4,"cell_delta_c");printf("%.1f",tmax-tmin);sep();key(4,"mosfet_c");printf("%.1f",s->mosfet_t);sep();key(4,"ambient_c");printf("%.1f",s->ambient_t);sep();key(4,"overall_min_c");printf("%.1f",omin);sep();key(4,"overall_max_c");printf("%.1f",omax);sep();key(4,"overall_average_c");printf("%.1f",osum/oc);nl();indent(2);putchar('}');sep();
    key(2,"protocol");putchar('{');nl();key(4,"manufacturer");printf("93");sep();key(4,"service_uuid");json_string("FFFA");sep();key(4,"tx_uuid");json_string("FFFB");sep();key(4,"rx_uuid");json_string("FFFC");sep();key(4,"att_mtu");printf("%u",s->mtu);sep();key(4,"service_handle_start");printf("%u",s->svc_start);sep();key(4,"service_handle_end");printf("%u",s->svc_end);sep();key(4,"tx_handle");printf("%u",s->tx);sep();key(4,"rx_handle");printf("%u",s->rx);sep();key(4,"cccd_handle");printf("%u",s->cccd);sep();key(4,"frame_length");printf("%zu",s->frame_len);sep();key(4,"crc_ok");fputs(s->crc_ok?"true":"false",stdout);sep();key(4,"register_start");printf("0");sep();key(4,"register_count");printf("72");sep();key(4,"write_parameters_supported");fputs("false",stdout);nl();indent(2);putchar('}');
    if(g_include_raw){sep();key(2,"raw");putchar('{');nl();key(4,"problem_register");printf("%u",(uint16_t)~s->problem);sep();key(4,"mosfet_register");printf("%u",s->mosfet);sep();key(4,"balancer_register");printf("%u",s->balancer);sep();key(4,"frame_hex");print_hex_string(s->raw,s->frame_len);nl();indent(2);putchar('}');}
    nl();putchar('}');putchar('\n');
}

static bool recoverable_error(int r){return r==-EIO||r==-ETIMEDOUT||r==-EBUSY||r==-ENODEV||r==-ECONNRESET||r==-ENOTCONN||r==-EHOSTDOWN;}

static int recover_adapter_for_attempt(int attempt)
{
    if(!g_recover||attempt<=0)return 0;
    (void)mgmt_stop_discovery_now();
    return adapter_reset_level(attempt>=2);
}

static int read_sample_once(const char *mac,const char *name,int atype,struct sample *s)
{
    struct att_conn ac;int r=att_connect_auto(mac,atype,&ac);if(r<0)return r;dbgf("ATT MTU: %u\n",ac.mtu);r=setup_saihang(&ac);if(r<0){close(ac.fd);return r;}uint8_t f[MAX_FRAME];size_t n=0;r=send_modbus_read(&ac,3,0,72,f,sizeof(f),&n);if(r==0)r=decode_sample(mac,name,&ac,f,n,s);close(ac.fd);return r;
}

static int read_sample_recover(const char *mac,const char *name,int atype,struct sample *s)
{
    int r=-EIO;
    for(int a=0;a<= (g_recover?g_recover_retries:0);a++){if(a>0){dbgf("read %s failed (%s); recovery attempt %d/%d\n",mac,strerror(-r),a,g_recover_retries);if(recover_adapter_for_attempt(a)<0)continue;}r=read_sample_once(mac,name,atype,s);if(r==0||!recoverable_error(r))break;}
    return r;
}

static void print_error_json(const char *addr,const char *msg,int code)
{
    putchar('{');fputs("\"address\":",stdout);json_string(addr);fputs(",\"ok\":false,\"error\":",stdout);json_string(msg);printf(",\"errno\":%d}",-code);
}

static int parse_addr_type(const char *s){if(!strcmp(s,"auto"))return 0;if(!strcmp(s,"public"))return BDADDR_LE_PUBLIC;if(!strcmp(s,"random"))return BDADDR_LE_RANDOM;return -1;}

static int cmd_scan(int argc,char **argv)
{
    int timeout=8;bool all=false;static struct option o[]={{"timeout",required_argument,0,'t'},{"all",no_argument,0,'a'},{0,0,0,0}};optind=1;int c;while((c=getopt_long(argc,argv,"t:a",o,NULL))!=-1){if(c=='t')timeout=atoi(optarg);else if(c=='a')all=true;else return 2;}struct discovered d[MAX_DEVICES];int n,r=scan_devices(timeout,all,d,&n);if(r==-EBUSY&&g_recover){dbgf("discovery busy; stopping stale discovery and retrying\n");(void)mgmt_stop_discovery_now();msleep_int(300);r=scan_devices(timeout,all,d,&n);}if(r<0){fprintf(stderr,"scan failed: %s\n",strerror(-r));return 1;}putchar('[');for(int i=0;i<n;i++){if(i)putchar(',');if(g_pretty){putchar('\n');indent(2);}printf("{\"address\":");json_string(d[i].addr);printf(",\"address_type\":");json_string(d[i].addr_type==BDADDR_LE_RANDOM?"random":"public");printf(",\"rssi\":%d,\"name\":",d[i].rssi);json_string(d[i].name);printf(",\"service_fffa\":%s}",d[i].has_fffa?"true":"false");}if(g_pretty&&n)putchar('\n');putchar(']');putchar('\n');return 0;
}

static int cmd_read(int argc,char **argv)
{
    int at=0;static struct option o[]={{"addr-type",required_argument,0,'t'},{0,0,0,0}};optind=1;int c;while((c=getopt_long(argc,argv,"t:",o,NULL))!=-1){if(c=='t'){at=parse_addr_type(optarg);if(at<0)return 2;}else return 2;}if(optind>=argc)return 2;struct sample s;int r=read_sample_recover(argv[optind],"",at,&s);if(r<0){fprintf(stderr,"read failed: %s\n",strerror(-r));return 1;}print_sample_json(&s);return 0;
}

static int cmd_read_all(int argc,char **argv)
{
    int timeout=8;static struct option o[]={{"timeout",required_argument,0,'t'},{0,0,0,0}};optind=1;int c;while((c=getopt_long(argc,argv,"t:",o,NULL))!=-1){if(c=='t')timeout=atoi(optarg);else return 2;}struct discovered d[MAX_DEVICES];int n,r=scan_devices(timeout,false,d,&n);if(r==-EBUSY&&g_recover){(void)mgmt_stop_discovery_now();msleep_int(300);r=scan_devices(timeout,false,d,&n);}if(r<0){fprintf(stderr,"scan failed: %s\n",strerror(-r));return 1;}msleep_int(200);putchar('[');for(int i=0;i<n;i++){if(i)putchar(',');if(g_pretty){putchar('\n');indent(2);}struct sample s;r=read_sample_recover(d[i].addr,d[i].name,d[i].addr_type,&s);if(r<0)print_error_json(d[i].addr,strerror(-r),r);else print_sample_json(&s);}if(g_pretty&&n)putchar('\n');putchar(']');putchar('\n');return 0;
}

static int cmd_watch(int argc,char **argv)
{
    int at=0,interval=5;long count=0;static struct option o[]={{"addr-type",required_argument,0,'t'},{"interval",required_argument,0,'i'},{"count",required_argument,0,'c'},{0,0,0,0}};optind=1;int c;while((c=getopt_long(argc,argv,"t:i:c:",o,NULL))!=-1){if(c=='t'){at=parse_addr_type(optarg);if(at<0)return 2;}else if(c=='i')interval=atoi(optarg);else if(c=='c')count=atol(optarg);else return 2;}if(optind>=argc||interval<1||interval>3600||count<0)return 2;const char *mac=argv[optind];long done=0;while(count==0||done<count){struct sample s;int r=read_sample_recover(mac,"",at,&s);if(r<0){print_error_json(mac,strerror(-r),r);putchar('\n');}else print_sample_json(&s);fflush(stdout);done++;if(count&&done>=count)break;sleep(interval);}return 0;
}

static int cmd_registers(int argc,char **argv)
{
    int at=0,fn=3;static struct option o[]={{"addr-type",required_argument,0,'t'},{"function",required_argument,0,'f'},{0,0,0,0}};optind=1;int c;while((c=getopt_long(argc,argv,"t:f:",o,NULL))!=-1){if(c=='t'){at=parse_addr_type(optarg);if(at<0)return 2;}else if(c=='f')fn=atoi(optarg);else return 2;}if(argc-optind<3)return 2;const char *mac=argv[optind++];long start=strtol(argv[optind++],NULL,0),count=strtol(argv[optind++],NULL,0);if(start<0||start>65535||count<1||count>125||(fn!=3&&fn!=4))return 2;uint8_t frame[MAX_FRAME];size_t flen=0;int r=-EIO;for(int a=0;a<=(g_recover?g_recover_retries:0);a++){if(a>0&&recover_adapter_for_attempt(a)<0)continue;struct att_conn ac;r=att_connect_auto(mac,at,&ac);if(r<0){if(!recoverable_error(r))break;continue;}r=setup_saihang(&ac);if(r==0)r=send_modbus_read(&ac,fn,start,count,frame,sizeof(frame),&flen);close(ac.fd);if(r==0||!recoverable_error(r))break;}if(r<0){fprintf(stderr,"register read failed: %s\n",strerror(-r));return 1;}size_t bytes=frame[4];putchar('{');printf("\"address\":");json_string(mac);printf(",\"function\":%d,\"start\":%ld,\"count\":%ld,\"registers\":[",fn,start,count);for(long i=0;i<count&&(size_t)(i*2+1)<bytes;i++){if(i)putchar(',');printf("%u",be16_get(frame+5+i*2));}putchar(']');if(g_include_raw){printf(",\"frame_hex\":");print_hex_string(frame,flen);}putchar('}');putchar('\n');return 0;
}

static void print_adapter_status_json(void)
{
    uint16_t idx;int r=resolve_current_index(&idx);if(r<0){printf("{\"ok\":false,\"error\":");json_string(strerror(-r));printf("}\n");return;}struct adapter_sysfs a;r=adapter_sysfs_info(idx,&a);bdaddr_t ba;char addr[18]="";if(hci_devba(idx,&ba)==0)ba2str(&ba,addr);char auth[16]="",driver[PATH_MAX]="";if(r==0){char p[PATH_MAX];snprintf(p,sizeof(p),"/sys/bus/usb/devices/%s/authorized",a.usb_device);(void)read_small_file(p,auth,sizeof(auth));snprintf(p,sizeof(p),"/sys/bus/usb/devices/%s/driver",a.usb_interface);char rr[PATH_MAX];if(realpath(p,rr)){const char *b=strrchr(rr,'/');snprintf(driver,sizeof(driver),"%s",b?b+1:rr);}}printf("{\"ok\":true,\"index\":%u,\"hci\":",idx);char h[32];snprintf(h,sizeof(h),"hci%u",idx);json_string(h);printf(",\"address\":");json_string(addr);printf(",\"sysfs\":");json_string(r==0?a.device_path:"");printf(",\"usb_interface\":");json_string(r==0?a.usb_interface:"");printf(",\"usb_device\":");json_string(r==0?a.usb_device:"");printf(",\"usb_vid\":");json_string(r==0?a.vendor:"");printf(",\"usb_pid\":");json_string(r==0?a.product:"");printf(",\"driver\":");json_string(driver);printf(",\"authorized\":");json_string(auth);printf(",\"auto_recovery\":%s,\"recover_retries\":%d}\n",g_recover?"true":"false",g_recover_retries);
}

static int cmd_adapter(int argc,char **argv)
{
    if(argc<2||!strcmp(argv[1],"status")){print_adapter_status_json();return 0;}if(!strcmp(argv[1],"reset")){bool hard=false;for(int i=2;i<argc;i++){if(!strcmp(argv[i],"--hard"))hard=true;else return 2;}(void)mgmt_stop_discovery_now();int r=adapter_reset_level(hard);if(r<0){fprintf(stderr,"adapter reset failed: %s\n",strerror(-r));return 1;}print_adapter_status_json();return 0;}return 2;
}

static void cmd_capabilities(void){printf("{\"application\":\"saihang-bms\",\"version\":\"%s\",\"device_family\":\"Saihang SH-series / Anenji / Aninerel\",\"discovery\":true,\"read_status\":true,\"read_registers\":true,\"adapter_status\":true,\"adapter_soft_reset\":true,\"adapter_hard_reset\":true,\"automatic_recovery\":true,\"write_parameters\":false,\"reason\":\"No verified public Saihang write register map is implemented\"}\n",APP_VERSION);}

static void usage(FILE *f)
{
    fprintf(f,"saihang-bms %s - Saihang/Anenji SH-series BLE BMS CLI\n\nUsage:\n  saihang-bms [global options] scan [--timeout SEC] [--all]\n  saihang-bms [global options] read [--addr-type auto|public|random] MAC\n  saihang-bms [global options] read-all [--timeout SEC]\n  saihang-bms [global options] watch [--interval SEC] [--count N] MAC\n  saihang-bms [global options] registers [--function 3|4] MAC START COUNT\n  saihang-bms [global options] adapter status\n  saihang-bms [global options] adapter reset [--hard]\n  saihang-bms capabilities\n\nGlobal options:\n  -p, --pretty\n  -r, --raw\n  -v, --verbose\n  -i, --adapter N\n  -T, --io-timeout MS\n      --no-recover\n      --recover-retries N\n      --recover-delay MS\n  -V, --version\n",APP_VERSION);
}

int main(int argc,char **argv)
{
    static struct option gopts[]={{"pretty",no_argument,0,'p'},{"raw",no_argument,0,'r'},{"verbose",no_argument,0,'v'},{"adapter",required_argument,0,'i'},{"io-timeout",required_argument,0,'T'},{"version",no_argument,0,'V'},{"no-recover",no_argument,0,1000},{"recover-retries",required_argument,0,1001},{"recover-delay",required_argument,0,1002},{"help",no_argument,0,'h'},{0,0,0,0}};int c;while((c=getopt_long(argc,argv,"+prvi:T:Vh",gopts,NULL))!=-1){switch(c){case 'p':g_pretty=true;break;case 'r':g_include_raw=true;break;case 'v':g_verbose=true;break;case 'i':g_adapter=atoi(optarg);if(g_adapter<0)return 2;break;case 'T':g_timeout_ms=atoi(optarg);if(g_timeout_ms<100)return 2;break;case 'V':printf("%s\n",APP_VERSION);return 0;case 1000:g_recover=false;break;case 1001:g_recover_retries=atoi(optarg);if(g_recover_retries<0||g_recover_retries>2)return 2;break;case 1002:g_recover_delay_ms=atoi(optarg);if(g_recover_delay_ms<0||g_recover_delay_ms>30000)return 2;break;case 'h':usage(stdout);return 0;default:return 2;}}if(optind>=argc){usage(stderr);return 2;}const char *cmd=argv[optind];argc-=optind;argv+=optind;if(!strcmp(cmd,"scan"))return cmd_scan(argc,argv);if(!strcmp(cmd,"read"))return cmd_read(argc,argv);if(!strcmp(cmd,"read-all"))return cmd_read_all(argc,argv);if(!strcmp(cmd,"watch"))return cmd_watch(argc,argv);if(!strcmp(cmd,"registers"))return cmd_registers(argc,argv);if(!strcmp(cmd,"adapter"))return cmd_adapter(argc,argv);if(!strcmp(cmd,"capabilities")){cmd_capabilities();return 0;}fprintf(stderr,"unknown command: %s\n",cmd);return 2;
}
