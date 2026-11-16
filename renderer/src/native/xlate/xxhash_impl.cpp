// xxHash's implementation for the renderer and the shader translator built from the SDK's
// sources (shader_xlate.cmake): the SDK's own copies are inside its DLLs and not exported.
#define XXH_IMPLEMENTATION
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
