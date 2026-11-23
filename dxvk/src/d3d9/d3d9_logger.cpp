#include "../util/log/log.h"

namespace dxvk {
  // Only d3d9.dll defines this logger: ddraw.dll links the same D3D9 code with its own
  Logger Logger::s_instance("d3d9.log");
}
