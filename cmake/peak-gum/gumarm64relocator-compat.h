#ifndef PEAK_GUMARM64RELOCATOR_COMPAT_H
#define PEAK_GUMARM64RELOCATOR_COMPAT_H

/*
 * Copyright (C) 2014-2026 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */


#include "frida-gum.h"



G_BEGIN_DECLS

GUM_API guint8 * gum_interceptor_peak_read_relocation_code (
    gconstpointer address, gsize length, gsize * n_bytes_read);
GUM_API gboolean gum_interceptor_peak_relocation_range_available (
    gconstpointer address, gsize length);
GUM_API gboolean gum_interceptor_peak_relocation_target_available (
    gconstpointer target);
GUM_API gboolean gum_interceptor_peak_relocation_is_opaque_entry (
    gconstpointer address);

typedef struct _GumArm64Relocator GumArm64Relocator;

struct _GumArm64Relocator
{
  volatile gint ref_count;

  csh capstone;

  const guint8 * input_start;
  const guint8 * input_cur;
  GumAddress input_pc;
  cs_insn ** input_insns;
  GumArm64Writer * output;

  guint inpos;
  guint outpos;

  gboolean eob;
  gboolean eoi;
};

GUM_API GumArm64Relocator * gum_arm64_relocator_new (gconstpointer input_code,
    GumArm64Writer * output);
GUM_API GumArm64Relocator * gum_arm64_relocator_ref (
    GumArm64Relocator * relocator);
GUM_API void gum_arm64_relocator_unref (GumArm64Relocator * relocator);

GUM_API void gum_arm64_relocator_init (GumArm64Relocator * relocator,
    gconstpointer input_code, GumArm64Writer * output);
GUM_API void gum_arm64_relocator_clear (GumArm64Relocator * relocator);

GUM_API void gum_arm64_relocator_reset (GumArm64Relocator * relocator,
    gconstpointer input_code, GumArm64Writer * output);

GUM_API guint gum_arm64_relocator_read_one (GumArm64Relocator * self,
    const cs_insn ** instruction);

GUM_API cs_insn * gum_arm64_relocator_peek_next_write_insn (
    GumArm64Relocator * self);
GUM_API gpointer gum_arm64_relocator_peek_next_write_source (
    GumArm64Relocator * self);
GUM_API void gum_arm64_relocator_skip_one (GumArm64Relocator * self);
GUM_API gboolean gum_arm64_relocator_write_one (GumArm64Relocator * self);
GUM_API void gum_arm64_relocator_write_all (GumArm64Relocator * self);

GUM_API gboolean gum_arm64_relocator_eob (GumArm64Relocator * self);
GUM_API gboolean gum_arm64_relocator_eoi (GumArm64Relocator * self);

GUM_API gboolean gum_arm64_relocator_can_relocate (gpointer address,
    guint min_bytes, GumRelocationScenario scenario, GumRelocationPolicy policy,
    guint * maximum, arm64_reg * available_scratch_reg);
GUM_API guint gum_arm64_relocator_relocate (gpointer from, guint min_bytes,
    gpointer to);

G_END_DECLS

#endif /* PEAK_GUMARM64RELOCATOR_COMPAT_H */
