#include "utils/env_parser.h"

int
main()
{
    PeakEnvWarningState state{};
    PeakEnvUnsignedSchema schema{};

    schema.warning_emitted = &state;
    return 0;
}
