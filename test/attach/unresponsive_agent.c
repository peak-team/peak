/* Valid Frida entrypoint deliberately provides no control socket. */
__attribute__((visibility("default"))) void
peak_attach_agent_main(const char *configuration, int *stay_resident)
{
    (void)configuration;
    *stay_resident = 1;
}
