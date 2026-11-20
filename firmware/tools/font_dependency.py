"""Track the binary included by the assembler when building incrementally."""
Import("env")

env.Depends("$BUILD_DIR/src/fontdata.S.o", "$PROJECT_DIR/assets/fonts.bin")
