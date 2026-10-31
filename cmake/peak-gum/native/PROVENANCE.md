# Pinned Frida Gum source inputs

Upstream: https://github.com/frida/frida-gum, tag `17.15.3`, commit `e5b0d7d6a9a83da1750b6fc654006a714b81a890`.
License: wxWindows Library Licence Version 3.1, reproduced in `COPYING.frida-gum-17.15.3`; original copyright headers remain in each imported source/header.

The two backend C files preserve the audited upstream executable bodies except the reviewed pre-full-context native entry emission (and ARM-only capability marker). Redundant internal include directives are replaced by the selected devkit's amalgamated header plus exact private/reader/relocator declarations. Those declaration shims retain the upstream declarations and remove only redundant includes. The new helper assembly and checked offset constants are PEAK code.

The selected original archive member is `backend-x86_guminterceptor-x86.c.o` or `backend-arm64_guminterceptor-arm64.c.o`. CMake compiles that member from these sources, replaces it before existing PEAK ELF/import routing, adds the architecture helper, and compiles `native-layout.c` solely for actual-type static assertions. No precompiled objects, target executable offset generator, source download or developer paths are required.

This optimization is selected only for the existing Linux x86_64/AArch64 source-overlay providers. Other providers/platforms retain their existing C path. The capability macro and actual native-hit calibration are both required before native rent policy becomes eligible.

Original upstream source SHA256 values:

- `arch-arm64/gumarm64reader.h`: `3e30598a2620159ec7ef19bf0b969655c282c95a1f8ad95857d1132a8ea08dac`
- `arch-arm64/gumarm64relocator.h`: `234d9ae43e5cb1b7de9528789cccc023145c26dffa0b8fe585169534b0adeaab`
- `arch-x86/gumx86reader.h`: `fabdacc7fa187b8ddbaf52cd2299d40f1880c8591334cb96e19c417bab3f1da3`
- `arch-x86/gumx86relocator.h`: `036d8b68e2b81a561374657b6818203f98fb2a7575bda414f1005a6b0611d603`
- `backend-arm64/guminterceptor-arm64.c`: `77e20e3b92e0fc2ad3ffe8a562add66df97b47257e3709d9de089ccb372235fd`
- `backend-x86/guminterceptor-x86.c`: `7b8821e6eb6b387675c1f2efaf404005e2e9eeadb0877e5c2e77d1130481c450`
- `guminterceptor-priv.h`: `e483e7b62c316865a4c27bf56819da9a34664f9585d971c5595af87bdd505bea`

Integrated source SHA256 values (backend/headers/helper/layout):

- `include/internal/native_thread_pointer.h`: `e16ef948564251587f841890695e61ce52d47dd9243f0a3e0acfe0682a17baed`

- `COPYING.frida-gum-17.15.3`: `00764fcc23f0f2ac6d2619a1f9192e55176ecaaa525e76a47aaabc5d87fb087f`
- `gum_peak_native_gate.S`: `d22ef6381553739778a5db79a810e2d5704ae85a0777e81d537f0ba500f4d463`
- `gumarm64reader.h`: `46d7e8ec2d7b7647da5912dd29d0fd94c9d8dcea44051d68b72aeaa8e7e21112`
- `gumarm64relocator.h`: `d57924ba9c54103eaffb8c685243d83bdcaed646a51c17a1dfafb960f4ec719a`
- `guminterceptor-arm64-native.c`: `89f49142be22ce0c410ddb0001f252874b41c891ca4699a452cdde34a2c6dc8f`
- `guminterceptor-private-exact.h`: `865a0c3d6ac32e6c861445fa2f43cbbd17aae31e6f2c9823efdc74944a151a55`
- `guminterceptor-x86-native.c`: `b8cbe3723e5eb6e5d1eb79c3bcca9b2ac0a7a602c09db21467fa060835978d1a`
- `gumx86reader.h`: `ae6888acc7824ae67fa0fa7f4d1c9f82334f0694e3239405575cddc1891821c1`
- `gumx86relocator.h`: `582dcc9e2d90e18de84495f60c6d5789c349f2051def49e4abda2edaa68e85b2`
- `native-layout.c`: `e9f31c94a3a349d61a37ed63cace036fe46e5f5d06289b9252253f3f73a35347`
- `native-offsets.h`: `8ed5ae33282785973feddd0d5b7b445a32ab33cc0e038e8316e35a590f75e2be`
