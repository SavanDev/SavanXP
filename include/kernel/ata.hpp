#pragma once

#include "kernel/block.hpp"

namespace ata {

// Driver de los 4 slots ATA PIO (IDE primario/secundario, master/slave). Su
// enumerate los sondea con IDENTIFY y registra en block:: los que responden.
const block::Driver& driver();

// Diagnostico del fallo intermitente de las escrituras largas: imprime una vez
// por que fallo un sondeo (presupuesto de spins agotado o error del
// dispositivo). Ver kernel/ata.cpp.
void report_poll_failures();
// Curva de esperas que SI funcionan: cuanto margen le queda al presupuesto de
// spins. No necesita que falle nada para decir algo.
void report_poll_headroom();

} // namespace ata
