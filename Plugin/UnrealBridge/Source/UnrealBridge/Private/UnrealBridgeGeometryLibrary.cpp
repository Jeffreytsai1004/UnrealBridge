#include "UnrealBridgeGeometryLibrary.h"

#include "Misc/EngineVersionComparison.h"

#if !UE_VERSION_OLDER_THAN(5, 7, 0)

// Geometry Script API headers land in Phase 2 alongside the M4
// handle-pool + asset-I/O UFUNCTIONs. This TU is intentionally empty
// until then — Phase 1 only proves the new module dependencies wire up
// without breaking the build.

namespace BridgeGeometryImpl
{
	// Handle pool, asset resolvers, and string→enum mappers (boolean op,
	// UV unwrap method, etc.) land here in Phase 2 / Phase 3. Use a named
	// namespace per feedback_no_unnamed_namespace.
}

#endif // !UE_VERSION_OLDER_THAN(5, 7, 0)
