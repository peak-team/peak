#if defined(__aarch64__) && !defined(PEAK_GUM_TEST_ARM64_STAGED_REDIRECT)
extern gboolean gum_arm64_relocator_can_relocate(gpointer, guint,
    GumRelocationScenario, GumRelocationPolicy, guint *, arm64_reg *);
#endif

static GumRedirectWriteResult
peak_gum_write_staged_redirect (const GumRedirectWriteDetails * details,
                                gpointer user_data)
{
  GumPeakStagedMutation * stage = user_data;
#if (defined(__x86_64__) || defined(__amd64__)) && !defined(PEAK_GUM_TEST_ARM64_STAGED_REDIRECT)
  GumX86Writer * writer = details->writer;
  gint64 distance = (gint64) GUM_ADDRESS (details->target) -
      (gint64) writer->pc - 5;
  guint needed = (distance >= G_MININT32 && distance <= G_MAXINT32) ? 5 : 14;
  if (details->capacity < needed ||
      !gum_x86_writer_put_jmp_address (writer, GUM_ADDRESS (details->target)))
    return GUM_REDIRECT_DECLINED;
  stage->redirect_size = gum_x86_writer_offset (writer);
  if (stage->redirect_size > details->capacity)
    return GUM_REDIRECT_DECLINED;
#else
  GumArm64Writer * writer = details->writer;
  guint needed = gum_arm64_writer_can_branch_directly_between (writer,
      writer->pc, GUM_ADDRESS (details->target)) ? 4 : 16;
  arm64_reg scratch = details->scratch_register;
  guint maximum = 0;
  if (details->capacity < needed ||
      !gum_arm64_relocator_can_relocate (GSIZE_TO_POINTER (writer->pc), needed,
          GUM_SCENARIO_ONLINE, GUM_RELOCATION_CHECKED, &maximum, &scratch) ||
      maximum < needed || scratch == ARM64_REG_INVALID ||
      scratch != (arm64_reg) details->scratch_register)
    return GUM_REDIRECT_DECLINED;
  if (needed == 4) {
    if (!gum_arm64_writer_put_b_imm (writer, GUM_ADDRESS (details->target)))
      return GUM_REDIRECT_DECLINED;
  } else {
    if (!gum_arm64_writer_put_ldr_reg_address (writer, scratch,
            GUM_ADDRESS (details->target)) ||
        !gum_arm64_writer_put_br_reg (writer, scratch))
      return GUM_REDIRECT_DECLINED;
  }
  /* Literal-pool addresses are part of the entry patch, not just the LDR
   * and BR instructions. Resolve them before recording the copied length. */
  if (!gum_arm64_writer_flush (writer))
    return GUM_REDIRECT_DECLINED;
  stage->redirect_size = gum_arm64_writer_offset (writer);
  if (stage->redirect_size > details->capacity)
    return GUM_REDIRECT_DECLINED;
#endif
  return GUM_REDIRECT_WRITTEN;
}
