/* dsp/core_ident.h: the reply of our capture core's identity op (E2). */
#include <stdio.h>
#include <string.h>
#include "core_ident.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    /* what core D6 sends (fpga/core/tb/tb.sv prints the same bytes) */
    const uint8_t d6[CORE_IDENT_REPLY_LEN] = { 0x80, 0x00, 0x4F, 0x53, 0x01, 0xD6, 0x1F, 0x0A, 0xDE };
    core_ident_t id = core_ident_parse(d6);
    CHECK(id.valid && id.proto == 1 && id.tag == 0xD6 && id.caps == 0x1F && id.depth_log2 == 10);
    CHECK(core_ident_has(&id, FPGA_CORE_CAP_FORCE));
    CHECK(d6[5] == FPGA_CORE_TAG && d6[6] == FPGA_CORE_CAPS && d6[7] == FPGA_CORE_ADDR_W);
    /* core D5: the same protocol, without op 09 */
    const uint8_t d5[CORE_IDENT_REPLY_LEN] = { 0x80, 0x00, 0x4F, 0x53, 0x01, 0xD5, 0x0F, 0x0A, 0xCD };
    id = core_ident_parse(d5);
    CHECK(id.valid && id.proto == 1 && id.tag == 0xD5 && id.caps == 0x0F && id.depth_log2 == 10);
    CHECK(core_ident_has(&id, FPGA_CORE_CAP_SEAM));
    CHECK(core_ident_has(&id, FPGA_CORE_CAP_SEAM | FPGA_CORE_CAP_LOCKRST));
    CHECK(!core_ident_has(&id, FPGA_CORE_CAP_FORCE));

    /* a core that does not know the op (D3, D4): 80 then zeros */
    const uint8_t old[CORE_IDENT_REPLY_LEN] = { 0x80, 0, 0, 0, 0, 0, 0, 0, 0 };
    id = core_ident_parse(old);
    CHECK(!id.valid && !core_ident_has(&id, FPGA_CORE_CAP_SEAM));

    /* a dead or floating line */
    uint8_t buf[CORE_IDENT_REPLY_LEN];
    memset(buf, 0xFF, sizeof buf); CHECK(!core_ident_parse(buf).valid);
    memset(buf, 0x00, sizeof buf); CHECK(!core_ident_parse(buf).valid);

    /* one wrong bit anywhere in the checked bytes is refused */
    for (unsigned i = 0; i < CORE_IDENT_REPLY_LEN; i++)
        for (unsigned b = 0; b < 8; b++) {
            memcpy(buf, d5, sizeof buf);
            buf[i] ^= (uint8_t)(1u << b);
            CHECK(!core_ident_parse(buf).valid);
        }

    /* a newer protocol with a correct check byte: reported, not trusted */
    memcpy(buf, d5, sizeof buf); buf[4] = 0x02; buf[8] ^= 0x03;
    id = core_ident_parse(buf);
    CHECK(!id.valid && id.proto == 2 && id.tag == 0xD5);

    /* capabilities are acted on only for a design we loaded that identified itself */
    CHECK(core_ident_trusted_caps(true, d5) == 0x0F);
    CHECK(core_ident_trusted_caps(false, d5) == 0);
    CHECK(core_ident_trusted_caps(true, old) == 0);
    CHECK(core_ident_trusted_caps(true, buf) == 0);          /* the newer protocol above */

    if (fails) { printf("test_core_ident: %d FAILED\n", fails); return 1; }
    printf("test_core_ident: all passed\n");
    return 0;
}
