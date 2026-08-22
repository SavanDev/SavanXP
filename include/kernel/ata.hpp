#pragma once

#include "kernel/block.hpp"

namespace ata {

// Driver de los 4 slots ATA PIO (IDE primario/secundario, master/slave). Su
// enumerate los sondea con IDENTIFY y registra en block:: los que responden.
const block::Driver& driver();

} // namespace ata
