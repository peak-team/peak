#include "frida-gum.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define PEAK_GUM_TEST_ARM64_STAGED_REDIRECT 1
struct _GumPeakStagedMutation { guint redirect_size; };
/* Isolate encoding from the separate relocator safety proof. */
static gboolean gum_arm64_relocator_can_relocate(gpointer address, guint size,
    GumRelocationScenario scenario, GumRelocationPolicy policy,
    guint *maximum, arm64_reg *scratch)
{
    (void)address; (void)scenario; (void)policy; (void)scratch;
    *maximum = size;
    return TRUE;
}
#include "gum_peak_staged_redirect.inc"
int main(void)
{
    guint8 bytes[32] = {0};
    GumArm64Writer writer;
    GumPeakStagedMutation stage = {0};
    GumAddress destination = G_GUINT64_CONSTANT(0x1234567890);
    gum_init_embedded();
    gum_arm64_writer_init(&writer, bytes);
    writer.pc = G_GUINT64_CONSTANT(0x7000000000);
    GumRedirectWriteDetails details = { .writer = &writer,
        .target = GSIZE_TO_POINTER(destination), .scratch_register = ARM64_REG_X16,
        .capacity = 16 };
    assert(peak_gum_write_staged_redirect(&details, &stage) == GUM_REDIRECT_WRITTEN);
    assert(stage.redirect_size == 16);
    guint64 literal;
    memcpy(&literal, bytes + 8, sizeof(literal));
    assert(literal == destination);
    assert(gum_arm64_writer_flush(&writer));
    assert(gum_arm64_writer_offset(&writer) == stage.redirect_size);
    gum_arm64_writer_clear(&writer);
    return 0;
}
