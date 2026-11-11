/*
 * usb_comp_desc.c — composite descriptor bytes + strict checker. See
 * usb_comp_desc.h.
 */
#include "usb_comp_desc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LO(x) ((uint8_t)((x) & 0xFFu))
#define HI(x) ((uint8_t)(((x) >> 8) & 0xFFu))

uint8_t usb_comp_device_desc[USB_COMP_DEVICE_LEN] __attribute__((aligned(4))) = {
    18, 0x01,
    0x00, 0x02,                         /* bcdUSB 2.00 */
    0xEF, 0x02, 0x01,                   /* misc / common / IAD: a composite device */
    USB_COMP_EP0_MPS,
    LO(USB_COMP_VID), HI(USB_COMP_VID),
    LO(USB_COMP_PID), HI(USB_COMP_PID),
    LO(USB_COMP_BCD_DEVICE), HI(USB_COMP_BCD_DEVICE),
    1, 2, 3,                            /* manufacturer, product, serial strings */
    1,                                  /* bNumConfigurations */
};

uint8_t usb_comp_config_desc[USB_COMP_CONFIG_LEN] __attribute__((aligned(4))) = {
    9, 0x02, LO(USB_COMP_CONFIG_LEN), HI(USB_COMP_CONFIG_LEN),
    USB_COMP_NUM_IF,
    1,                                  /* bConfigurationValue */
    0,
    0xC0,                               /* self powered (as the CDC build) */
    0x32,                               /* 100 mA */

    /* IAD: the CDC function owns interfaces 0..1 */
    8, 0x0B, USB_COMP_CDC_COMM_IF, 2, 0x02, 0x02, 0x01, 0,

    /* interface 0: CDC comm, ACM: byte for byte the CDC build's */
    9, 0x04, USB_COMP_CDC_COMM_IF, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, LO(USB_COMP_CDC_BCD), HI(USB_COMP_CDC_BCD),      /* header */
    5, 0x24, 0x01, 0x00, USB_COMP_CDC_DATA_IF,                       /* call management */
    4, 0x24, 0x02, 0x02,                                             /* ACM */
    5, 0x24, 0x06, USB_COMP_CDC_COMM_IF, USB_COMP_CDC_DATA_IF,       /* union */
    7, 0x05, USB_COMP_CDC_INT_EPT, 0x03,
    LO(USB_COMP_CDC_INT_MPS), HI(USB_COMP_CDC_INT_MPS), USB_COMP_CDC_INT_INTERVAL,

    /* interface 1: CDC data */
    9, 0x04, USB_COMP_CDC_DATA_IF, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, 0x05, USB_COMP_CDC_IN_EPT, 0x02, LO(USB_COMP_CDC_BULK_MPS), HI(USB_COMP_CDC_BULK_MPS), 0,
    7, 0x05, USB_COMP_CDC_OUT_EPT, 0x02, LO(USB_COMP_CDC_BULK_MPS), HI(USB_COMP_CDC_BULK_MPS), 0,

    /* interface 2: mass storage, SCSI transparent, bulk-only: the class
     * triple stock's own USB Sharing descriptor carries (08 06 50,
     * docs/re/parity2_static.md 4.5) */
    9, 0x04, USB_COMP_MSC_IF, 0, 2, 0x08, 0x06, 0x50, 0,
    7, 0x05, USB_COMP_MSC_IN_EPT, 0x02, LO(USB_COMP_MSC_MPS), HI(USB_COMP_MSC_MPS), 0,
    7, 0x05, USB_COMP_MSC_OUT_EPT, 0x02, LO(USB_COMP_MSC_MPS), HI(USB_COMP_MSC_MPS), 0,
};

/* ── Checker ───────────────────────────────────────────────────────────── */

#define MAX_IF   8
#define MAX_EPT  16
#define MAX_IAD  4

typedef struct {
    bool     seen;
    uint8_t  cls, sub, proto;
    uint8_t  n_ept_declared, n_ept_seen;
    uint8_t  n_int_in, n_bulk_in, n_bulk_out;
    bool     cdc_header, cdc_acm, cdc_union, cdc_cm;
    uint8_t  cs_count;
    uint8_t  union_slave, cm_data_if;
    int8_t   iad;           /* index of the IAD that covers it, -1 none */
} if_info_t;

static bool fail(char *err, size_t err_len, const char *fmt, ...)
{
    if (err != NULL && err_len > 0u) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, err_len, fmt, ap);
        va_end(ap);
    }
    return false;
}

static bool valid_bulk_mps(uint16_t m) { return m == 8u || m == 16u || m == 32u || m == 64u; }

static bool check_pma(const usb_pma_layout_t *pma, const uint8_t *ept_addr,
                      const uint16_t *ept_mps, size_t n_ept, char *err, size_t err_len)
{
    for (size_t i = 0; i < pma->nbufs; i++) {
        const usb_pma_buf_t *b = &pma->bufs[i];
        if (b->size == 0u)
            return fail(err, err_len, "PMA: buffer for 0x%02X has size 0", b->ept_addr);
        if (b->offset < pma->btable_size)
            return fail(err, err_len, "PMA: buffer for 0x%02X at 0x%03X overlaps the BTABLE (0..0x%03X)",
                        b->ept_addr, b->offset, pma->btable_size - 1u);
        if ((b->offset & 1u) != 0u)
            return fail(err, err_len, "PMA: buffer for 0x%02X at odd offset 0x%03X", b->ept_addr, b->offset);
        if ((uint32_t)b->offset + b->size > pma->pma_size)
            return fail(err, err_len, "PMA: buffer for 0x%02X (0x%03X+%u) runs past the %u-byte packet memory",
                        b->ept_addr, b->offset, b->size, pma->pma_size);
        if ((b->ept_addr & 0x80u) == 0u && b->size > 62u && (b->size % 32u) != 0u)
            return fail(err, err_len, "PMA: RX buffer for 0x%02X: %u bytes is not expressible (BL_SIZE=1 needs x32)",
                        b->ept_addr, b->size);
        for (size_t j = 0; j < i; j++) {
            const usb_pma_buf_t *c = &pma->bufs[j];
            if (c->ept_addr == b->ept_addr)
                return fail(err, err_len, "PMA: two buffers for 0x%02X", b->ept_addr);
            if (b->offset < c->offset + c->size && c->offset < b->offset + b->size)
                return fail(err, err_len, "PMA: buffers for 0x%02X (0x%03X+%u) and 0x%02X (0x%03X+%u) overlap",
                            b->ept_addr, b->offset, b->size, c->ept_addr, c->offset, c->size);
        }
    }
    /* every endpoint the descriptor declares, plus EP0 both ways */
    for (size_t k = 0; k < n_ept + 2u; k++) {
        uint8_t a = k < n_ept ? ept_addr[k] : (k == n_ept ? 0x00u : 0x80u);
        uint16_t m = k < n_ept ? ept_mps[k] : 64u;
        const usb_pma_buf_t *b = NULL;
        for (size_t i = 0; i < pma->nbufs; i++)
            if (pma->bufs[i].ept_addr == a) b = &pma->bufs[i];
        if (b == NULL)
            return fail(err, err_len, "PMA: endpoint 0x%02X has no packet-memory buffer", a);
        if (b->size < m)
            return fail(err, err_len, "PMA: endpoint 0x%02X buffer %u < wMaxPacketSize %u", a, b->size, m);
    }
    return true;
}

bool usb_desc_check(const uint8_t *dev, size_t dev_len,
                    const uint8_t *cfg, size_t cfg_len,
                    const usb_pma_layout_t *pma,
                    char *err, size_t err_len)
{
    if (err != NULL && err_len > 0u) err[0] = '\0';

    /* ── device descriptor ── */
    if (dev == NULL || dev_len != 18u || dev[0] != 18u || dev[1] != 0x01u)
        return fail(err, err_len, "device: bLength/bDescriptorType must be 18/0x01");
    const uint16_t bcd_usb = (uint16_t)(dev[2] | (dev[3] << 8));
    if (bcd_usb != 0x0200u && bcd_usb != 0x0110u)
        return fail(err, err_len, "device: bcdUSB 0x%04X (full-speed device: 0x0110 or 0x0200)", bcd_usb);
    if (dev[7] != 8u && dev[7] != 16u && dev[7] != 32u && dev[7] != 64u)
        return fail(err, err_len, "device: bMaxPacketSize0 %u", dev[7]);
    if (dev[17] != 1u)
        return fail(err, err_len, "device: bNumConfigurations %u (one configuration)", dev[17]);
    const bool dev_iad = dev[4] == 0xEFu;
    if (dev_iad && (dev[5] != 0x02u || dev[6] != 0x01u))
        return fail(err, err_len, "device: class EF needs subclass 02 protocol 01 (IAD)");
    if (!dev_iad && dev[4] != 0x00u && dev[4] != 0x02u)
        return fail(err, err_len, "device: unexpected bDeviceClass 0x%02X", dev[4]);

    /* ── configuration header ── */
    if (cfg == NULL || cfg_len < 9u || cfg[0] != 9u || cfg[1] != 0x02u)
        return fail(err, err_len, "config: bLength/bDescriptorType must be 9/0x02");
    const uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    if (total != cfg_len)
        return fail(err, err_len, "config: wTotalLength %u but %u bytes follow", total, (unsigned)cfg_len);
    const uint8_t n_if = cfg[4];
    if (n_if == 0u || n_if > MAX_IF)
        return fail(err, err_len, "config: bNumInterfaces %u", n_if);
    if (cfg[5] == 0u)
        return fail(err, err_len, "config: bConfigurationValue 0 is reserved for 'unconfigured'");
    if ((cfg[7] & 0x80u) == 0u || (cfg[7] & 0x1Fu) != 0u)
        return fail(err, err_len, "config: bmAttributes 0x%02X (bit 7 set, bits 0..4 zero)", cfg[7]);

    if_info_t ifs[MAX_IF];
    memset(ifs, 0, sizeof ifs);
    for (unsigned i = 0; i < MAX_IF; i++) ifs[i].iad = -1;
    uint8_t iad_first[MAX_IAD], iad_count[MAX_IAD], iad_cls[MAX_IAD], iad_sub[MAX_IAD], iad_proto[MAX_IAD];
    unsigned n_iad = 0;
    int pending_iad = -1;           /* an IAD must be followed by its first interface */
    uint8_t ept_addr[MAX_EPT];
    uint16_t ept_mps[MAX_EPT];
    unsigned n_ept = 0;
    int cur = -1;                   /* current interface number */
    unsigned n_if_seen = 0;

    size_t pos = 9;
    while (pos < cfg_len) {
        const uint8_t len = cfg[pos];
        if (len < 2u)
            return fail(err, err_len, "config+%u: bLength %u", (unsigned)pos, len);
        if (pos + len > cfg_len)
            return fail(err, err_len, "config+%u: descriptor (len %u) runs past wTotalLength", (unsigned)pos, len);
        const uint8_t type = cfg[pos + 1];
        const uint8_t *d = &cfg[pos];

        if (pending_iad >= 0 && type != 0x04u)
            return fail(err, err_len, "config+%u: IAD not immediately followed by its first interface", (unsigned)pos);

        switch (type) {
        case 0x0Bu:     /* interface association */
            if (len != 8u)
                return fail(err, err_len, "config+%u: IAD bLength %u (8)", (unsigned)pos, len);
            if (n_iad >= MAX_IAD)
                return fail(err, err_len, "config: too many IADs");
            if (d[3] < 2u)
                return fail(err, err_len, "config+%u: IAD bInterfaceCount %u (an association has >= 2)", (unsigned)pos, d[3]);
            iad_first[n_iad] = d[2]; iad_count[n_iad] = d[3];
            iad_cls[n_iad] = d[4]; iad_sub[n_iad] = d[5]; iad_proto[n_iad] = d[6];
            pending_iad = (int)n_iad;
            n_iad++;
            break;

        case 0x04u: {   /* interface */
            if (len != 9u)
                return fail(err, err_len, "config+%u: interface bLength %u (9)", (unsigned)pos, len);
            const uint8_t num = d[2];
            if (num >= MAX_IF || num >= n_if)
                return fail(err, err_len, "config+%u: interface %u >= bNumInterfaces %u", (unsigned)pos, num, n_if);
            if (d[3] != 0u)
                return fail(err, err_len, "config+%u: interface %u alternate setting %u (none expected)",
                            (unsigned)pos, num, d[3]);
            if (ifs[num].seen)
                return fail(err, err_len, "config+%u: interface %u declared twice", (unsigned)pos, num);
            if (cur >= 0 && ifs[cur].n_ept_seen != ifs[cur].n_ept_declared)
                return fail(err, err_len, "interface %d: bNumEndpoints %u but %u endpoint descriptors",
                            cur, ifs[cur].n_ept_declared, ifs[cur].n_ept_seen);
            if (pending_iad >= 0 && iad_first[pending_iad] != num)
                return fail(err, err_len, "config+%u: IAD bFirstInterface %u but next interface is %u",
                            (unsigned)pos, iad_first[pending_iad], num);
            if (pending_iad >= 0 && (iad_cls[pending_iad] != d[5] || iad_sub[pending_iad] != d[6] ||
                                     iad_proto[pending_iad] != d[7]))
                return fail(err, err_len, "IAD function %02X/%02X/%02X differs from interface %u's %02X/%02X/%02X",
                            iad_cls[pending_iad], iad_sub[pending_iad], iad_proto[pending_iad], num, d[5], d[6], d[7]);
            pending_iad = -1;
            ifs[num].seen = true;
            ifs[num].cls = d[5]; ifs[num].sub = d[6]; ifs[num].proto = d[7];
            ifs[num].n_ept_declared = d[4];
            for (unsigned a = 0; a < n_iad; a++)
                if (num >= iad_first[a] && num < (unsigned)iad_first[a] + iad_count[a]) {
                    if (ifs[num].iad >= 0)
                        return fail(err, err_len, "interface %u is in two IADs", num);
                    ifs[num].iad = (int8_t)a;
                }
            cur = num;
            n_if_seen++;
            break;
        }

        case 0x24u: {   /* CS_INTERFACE */
            if (cur < 0 || ifs[cur].cls != 0x02u)
                return fail(err, err_len, "config+%u: CDC functional descriptor outside a CDC comm interface", (unsigned)pos);
            if (ifs[cur].n_ept_seen != 0u)
                return fail(err, err_len, "config+%u: CDC functional descriptor after an endpoint", (unsigned)pos);
            if (len < 3u)
                return fail(err, err_len, "config+%u: CDC functional bLength %u", (unsigned)pos, len);
            if_info_t *f = &ifs[cur];
            const uint8_t sub = d[2];
            if (f->cs_count == 0u && sub != 0x00u)
                return fail(err, err_len, "interface %d: first CDC functional descriptor must be the header", cur);
            f->cs_count++;
            switch (sub) {
            case 0x00u:
                if (len != 5u) return fail(err, err_len, "interface %d: CDC header bLength %u (5)", cur, len);
                if (f->cdc_header) return fail(err, err_len, "interface %d: two CDC headers", cur);
                f->cdc_header = true;
                break;
            case 0x01u:
                if (len != 5u) return fail(err, err_len, "interface %d: call management bLength %u (5)", cur, len);
                f->cdc_cm = true;
                f->cm_data_if = d[4];
                break;
            case 0x02u:
                if (len != 4u) return fail(err, err_len, "interface %d: ACM bLength %u (4)", cur, len);
                f->cdc_acm = true;
                break;
            case 0x06u:
                if (len != 5u) return fail(err, err_len, "interface %d: union bLength %u (5: one subordinate)", cur, len);
                if (d[3] != (uint8_t)cur)
                    return fail(err, err_len, "interface %d: union bControlInterface %u", cur, d[3]);
                f->cdc_union = true;
                f->union_slave = d[4];
                break;
            default:
                return fail(err, err_len, "interface %d: unexpected CDC functional subtype 0x%02X", cur, sub);
            }
            break;
        }

        case 0x05u: {   /* endpoint */
            if (len != 7u)
                return fail(err, err_len, "config+%u: endpoint bLength %u (7)", (unsigned)pos, len);
            if (cur < 0)
                return fail(err, err_len, "config+%u: endpoint before any interface", (unsigned)pos);
            const uint8_t a = d[2];
            const uint8_t attr = d[3] & 0x03u;
            const uint16_t mps = (uint16_t)(d[4] | (d[5] << 8));
            if ((a & 0x0Fu) == 0u || (a & 0x70u) != 0u)
                return fail(err, err_len, "endpoint 0x%02X: bad address", a);
            if ((d[3] & 0xFCu) != 0u)
                return fail(err, err_len, "endpoint 0x%02X: bmAttributes 0x%02X (bulk/int: upper bits 0)", a, d[3]);
            for (unsigned k = 0; k < n_ept; k++)
                if (ept_addr[k] == a) return fail(err, err_len, "endpoint 0x%02X declared twice", a);
            if (n_ept >= MAX_EPT) return fail(err, err_len, "too many endpoints");
            if (attr == 0x02u) {
                if (!valid_bulk_mps(mps))
                    return fail(err, err_len, "endpoint 0x%02X: bulk wMaxPacketSize %u (FS: 8/16/32/64)", a, mps);
                if (d[6] != 0u)
                    return fail(err, err_len, "endpoint 0x%02X: bulk bInterval %u (0)", a, d[6]);
                if (a & 0x80u) ifs[cur].n_bulk_in++; else ifs[cur].n_bulk_out++;
            } else if (attr == 0x03u) {
                if (mps == 0u || mps > 64u)
                    return fail(err, err_len, "endpoint 0x%02X: interrupt wMaxPacketSize %u (FS: 1..64)", a, mps);
                if (d[6] == 0u)
                    return fail(err, err_len, "endpoint 0x%02X: interrupt bInterval 0", a);
                if (a & 0x80u) ifs[cur].n_int_in++;
                else return fail(err, err_len, "endpoint 0x%02X: unexpected interrupt OUT", a);
            } else {
                return fail(err, err_len, "endpoint 0x%02X: transfer type %u not used by this device", a, attr);
            }
            ept_addr[n_ept] = a;
            ept_mps[n_ept] = mps;
            n_ept++;
            ifs[cur].n_ept_seen++;
            break;
        }

        default:
            return fail(err, err_len, "config+%u: unexpected descriptor type 0x%02X", (unsigned)pos, type);
        }
        pos += len;
    }
    if (pending_iad >= 0)
        return fail(err, err_len, "config: IAD at the end with no interface");
    if (cur >= 0 && ifs[cur].n_ept_seen != ifs[cur].n_ept_declared)
        return fail(err, err_len, "interface %d: bNumEndpoints %u but %u endpoint descriptors",
                    cur, ifs[cur].n_ept_declared, ifs[cur].n_ept_seen);
    if (n_if_seen != n_if)
        return fail(err, err_len, "config: bNumInterfaces %u but %u interfaces", n_if, n_if_seen);
    for (unsigned i = 0; i < n_if; i++)
        if (!ifs[i].seen) return fail(err, err_len, "config: interface %u missing (numbers must be 0..%u)", i, n_if - 1u);

    for (unsigned a = 0; a < n_iad; a++) {
        if ((unsigned)iad_first[a] + iad_count[a] > n_if)
            return fail(err, err_len, "IAD %u: interfaces %u..%u beyond bNumInterfaces %u", a, iad_first[a],
                        iad_first[a] + iad_count[a] - 1u, n_if);
        for (unsigned i = iad_first[a]; i < (unsigned)iad_first[a] + iad_count[a]; i++)
            if (ifs[i].iad != (int8_t)a)
                return fail(err, err_len, "IAD %u: interface %u not covered", a, i);
    }
    if (dev_iad && n_iad == 0u)
        return fail(err, err_len, "device class EF/02/01 but no IAD in the configuration");

    /* ── per-function rules ── */
    for (unsigned i = 0; i < n_if; i++) {
        const if_info_t *f = &ifs[i];
        if (f->cls == 0x02u) {
            if (f->sub != 0x02u)
                return fail(err, err_len, "interface %u: CDC subclass 0x%02X (ACM 0x02)", i, f->sub);
            if (!f->cdc_header || !f->cdc_acm || !f->cdc_union || !f->cdc_cm)
                return fail(err, err_len, "interface %u: CDC ACM needs header, call management, ACM and union", i);
            if (f->union_slave >= n_if || ifs[f->union_slave].cls != 0x0Au)
                return fail(err, err_len, "interface %u: union subordinate %u is not a CDC data interface", i, f->union_slave);
            if (f->cm_data_if != f->union_slave)
                return fail(err, err_len, "interface %u: call management data interface %u != union subordinate %u",
                            i, f->cm_data_if, f->union_slave);
            if (f->n_int_in != 1u || f->n_bulk_in != 0u || f->n_bulk_out != 0u)
                return fail(err, err_len, "interface %u: CDC comm needs exactly one interrupt IN", i);
            if (n_if > 2u || dev_iad) {
                /* a CDC function in a multi-function device must be grouped */
                if (f->iad < 0)
                    return fail(err, err_len, "interface %u: CDC function without an IAD in a composite device", i);
                if (ifs[f->union_slave].iad != f->iad)
                    return fail(err, err_len, "interface %u: CDC data interface outside the CDC IAD", i);
                if (iad_first[f->iad] != i)
                    return fail(err, err_len, "IAD must start at the CDC comm interface %u", i);
            }
        } else if (f->cls == 0x0Au) {
            if (f->n_bulk_in != 1u || f->n_bulk_out != 1u || f->n_int_in != 0u)
                return fail(err, err_len, "interface %u: CDC data needs one bulk IN + one bulk OUT", i);
            bool owned = false;
            for (unsigned c = 0; c < n_if; c++)
                if (ifs[c].cls == 0x02u && ifs[c].cdc_union && ifs[c].union_slave == i) owned = true;
            if (!owned)
                return fail(err, err_len, "interface %u: CDC data interface no union refers to", i);
        } else if (f->cls == 0x08u) {
            if (f->sub != 0x06u || f->proto != 0x50u)
                return fail(err, err_len, "interface %u: MSC %02X/%02X (SCSI transparent / bulk-only: 06/50)",
                            i, f->sub, f->proto);
            if (f->n_bulk_in != 1u || f->n_bulk_out != 1u || f->n_int_in != 0u)
                return fail(err, err_len, "interface %u: MSC BOT needs one bulk IN + one bulk OUT", i);
            if (f->iad >= 0)
                return fail(err, err_len, "interface %u: single-interface MSC function inside an IAD", i);
        } else {
            return fail(err, err_len, "interface %u: unexpected class 0x%02X", i, f->cls);
        }
    }

    if (pma != NULL && !check_pma(pma, ept_addr, ept_mps, n_ept, err, err_len))
        return false;
    return true;
}
