#ifndef PEAK_ATTACH_PROTOCOL_H
#define PEAK_ATTACH_PROTOCOL_H

/* Frames contain a uint32 network byte order byte count, then UTF-8 GVariant
 * text without a trailing NUL. Configuration is an immutable PEAK_* mapping;
 * the uint32 field is reserved for protocol version 1 (must equal 1). */
#define PEAK_ATTACH_MAX_FRAME (1024U * 1024U)
#define PEAK_ATTACH_CONFIG_TYPE "(a{ss}u)"
#define PEAK_ATTACH_ACK_TYPE "(ss)"
#define PEAK_ATTACH_PROTOCOL_VERSION 1U
#define PEAK_ATTACH_ENTRYPOINT "peak_attach_agent_main"

#endif /* PEAK_ATTACH_PROTOCOL_H */
