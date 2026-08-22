# Changelog

Registro de cambios visibles por version para SavanXP.

Notas de corte:

- `v0.1.0` se reconstruyo de forma retroactiva desde el historial hasta `d822857`.
- `v0.1.1` cubre los cambios posteriores a `v0.1.0`, incluyendo el trabajo actual
  ya integrado en el arbol pero todavia no etiquetado en git.

## [Unreleased]

### Cambiado

- **Generacion de assets del desktop: de System.Drawing (GDI+) a Pillow.**
  Primer paso de la migracion del build a Linux: `tools/GenerateCursorAsset.ps1`,
  `tools/GenerateDesktopIconAssets.ps1` y `tools/GenerateDesktopSourceArt.ps1`
  dependian de `System.Drawing`, que en .NET moderno tira
  `PlatformNotSupportedException` fuera de Windows, y se ejecutaban en cada
  build (`Generate-CursorAsset`/`Generate-DesktopIconAssets` en `build.ps1`).
  Se reemplazaron por tres scripts Python + Pillow
  (`tools/gen_cursor_asset.py`, `tools/gen_desktop_icon_assets.py`,
  `tools/gen_desktop_source_art.py`), mismo formato de header C generado
  (`cursor_asset.h`, `desktop_icon_assets.h`) sin cambios de ABI. `build.ps1`
  ahora resuelve `python3`/`python` del `PATH` para invocarlos. Nuevo
  requisito de build en cualquier plataforma: `python3` + `pip install
  Pillow` (no forma parte del toolchain horneado por `bootstrap.ps1`).

- **Rutas con `\` literal portadas a `/` en los builds "aparte".**
  Segundo paso de la migracion a Linux: `Join-Path` no separa por `\` fuera de
  Windows (ahi es un caracter de nombre de archivo valido), asi que las rutas
  con `\` embebido a mano resolvian mal en `pwsh` sobre Linux. Se corrigieron
  las 16 ocurrencias en `subsystems/native/build.ps1`, 3 en
  `sdk/doomgeneric/build.ps1` y 1 cada una en `tools/new-user-app.ps1` y
  `tools/bootstrap.ps1`, al mismo patron de `/` que ya usaba `build.ps1`. Sin
  cambio de comportamiento en Windows (verificado corriendo los tres builds
  afectados de punta a punta).

- **`Build-Iso` ya no fuerza rutas cygdrive fuera de Windows.**
  Tercer paso de la migracion a Linux: `ConvertTo-CygwinPath` traducia
  siempre `C:\...` a `/cygdrive/c/...` para el `xorriso.exe` horneado
  (build de Cygwin), pero esa traduccion no tiene sentido con el `xorriso`
  nativo de Linux/macOS y ademas revienta (`No se pudo convertir`) porque el
  regex de unidad nunca matchea una ruta Linux. `Build-Iso` ahora chequea el
  host con la nueva `Test-IsWindowsHost` ($IsWindows, con fallback a `$true`
  para Windows PowerShell 5.1 donde esa variable no existe): en Windows sigue
  yendo por `ConvertTo-CygwinPath` igual que antes, fuera de Windows le pasa a
  `xorriso` la ruta absoluta tal cual. Verificado corriendo `.\build.ps1 iso`
  en Windows sin regresion.

- **`Build-Iso` compila el deployer `limine` con `make` fuera de Windows.**
  Cuarto y ultimo paso (por ahora) de la migracion a Linux: la rama
  `v10.x-binary` de Limine solo trae `limine.exe` prebuildeado para Windows;
  para el arranque BIOS de la ISO en Linux/macOS hace falta compilar el
  deployer `limine` a mano desde `limine.c` (el `Makefile` del propio repo de
  Limine lo arma con `cc -std=c99 limine.c -o limine`). `Build-Iso` ahora, si
  no encuentra `limine.exe` ni `limine` y `Test-IsWindowsHost` da `$false`,
  corre `make -C tools/limine` antes de tirar el error. Nuevo requisito de
  build en Linux/macOS (no en Windows): `make` + un compilador `cc` en el
  `PATH`.

- **Los backends de display y audio se eligen por registro de drivers, no por
  ramas en `kernel_main`.** Los dos HAL ya tenian la mitad polimorfica (las
  vtables `display::Backend` / `audio::Backend`), pero la seleccion vivia como
  un `if/else` en `kernel_main` que conocia a los cuatro drivers por nombre:
  sumar un backend obligaba a tocar el flujo de arranque. Ahora cada HAL tiene
  un registro (`register_driver` / `bind_best`) y cada driver se auto-describe
  con un `driver()` al lado de su `backend()`: un `Driver` con nombre,
  prioridad, `probe` (que hace su propio `initialize` y responde si reclama el
  hardware) y el getter del backend. `bind_best` corre los probes por prioridad
  descendente y corta en el primero que reclama, asi que un driver descartado
  por prioridad nunca llega a inicializarse — que es exactamente lo que hacia a
  mano el `else` al no tocar `fb_gpu` cuando virtio-gpu estaba presente.
  Prioridades: virtio-gpu 100 / framebuffer 10, virtio-sound 100 / ac97 50.
  `set_backend` sigue expuesto como mecanismo de bajo nivel para los caminos
  que atan un backend a mano (arranques headless, pruebas). Nuevos logs de
  boot: `display: backend '<nombre>'` y `audio: backend '<nombre>'`. Unico
  cambio de comportamiento: el probe de `fb_gpu` devuelve su `ready()` (que
  exige scanout lineal de 32bpp de tamano no nulo) en vez de reclamar el
  hardware siempre, asi que en una maquina sin framebuffer usable no queda
  ningun backend atado en lugar de quedar atado uno muerto — equivalente para
  los consumidores, que ya pasaban todos por `display::ready()`, y `gpu_device`
  registra `/dev/gpu0` igual en los dos casos. Verificado en las dos
  configuraciones de hardware: la base (VGA std + AC97) elige `framebuffer` +
  `ac97`, y `-Virtio` elige `virtio-gpu` + `virtio-sound` sin inicializar ac97;
  `build.ps1 smoke` (base y `-Virtio`) y `build.ps1 windowd-smoke` en PASS.

- **Los block devices tambien se registran por driver; ATA y ramdisk salen a su
  propia unidad de traduccion.** Mismo patron que display y audio, con una
  diferencia de fondo: aca los devices de todos los drivers **coexisten** (los
  ATA y el ramdisk del LiveCD a la vez), asi que no hay un `bind_best` que elija
  uno solo — `probe_all()` corre todos los `enumerate` por prioridad y cada
  driver registra lo que encuentra. Lo que faltaba en `block` era la otra mitad:
  ya tenia registro dinamico de devices (`register_ramdisk`), pero el despacho
  de I/O era un `switch` sobre un `enum Kind{ata, memory}` y los dos drivers
  vivian dentro de `block.cpp`, junto al core. Ahora `block::DeviceOps` es una
  vtable `read`/`write` por device con un context opaco que cada driver usa para
  lo suyo (el slot ATA, la base del ramdisk); `block::Driver` lleva nombre,
  prioridad y `enumerate`; y `kernel/ata.cpp` + `kernel/ramdisk.cpp` son
  unidades separadas, como `virtio_gpu.cpp`/`fb_gpu.cpp` para display. La
  prioridad define el orden de los indices de device (ata 100 antes que ramdisk
  10), que es lo que hace que un disco IDE persistente le gane a la imagen del
  LiveCD cuando `svfs` monta el primer SVFS2 valido. La validacion comun
  (indice, buffer, rango de LBA, permiso de escritura) subio al core y se hace
  una sola vez; los drivers se quedan solo con lo de su protocolo (el tope de
  255 sectores por comando del PIO de ATA). Cambios de API: `block::initialize`
  pasa a `block::probe_all` y `block::register_ramdisk` a
  `ramdisk::attach_image` (hay que llamarla antes de enumerar); el resto
  (`ready`/`device_count`/`device_info`/`read`/`write`/`kSectorSize`) no cambia,
  asi que `svfs` y los callers de `system_info` quedan intactos. Nuevo log de
  boot con la enumeracion: `block: N device(s) ata0(rw) livecd(rw)`. Verificado
  en las dos rutas de almacenamiento: con disco IDE enumera `ata0` + `livecd` en
  ese orden y `svfs` monta `ata0` (ejercita el PIO), con `build.ps1 smoke` y
  `build.ps1 windowd-smoke` en PASS; sin disco IDE (solo el modulo de Limine, el
  caso LiveCD) enumera `livecd`, `svfs` lo monta y la suite corre entera desde
  ahi — incluido leer `/disk/bin/gputest` y `/disk/bin/audiotest` — en PASS.

- **El driver del NIC sale del stack de red: `nic::Nic` + `kernel/rtl8139.cpp`.**
  Cierra el patron de registro de drivers en la ultima familia que faltaba, pero
  aca el registro era el ultimo paso y no el primero: `net.cpp` no tenia ninguna
  de las dos mitades. El rtl8139 estaba **fusionado** con el stack —
  `transmit_frame` se llamaba directo desde los caminos de ARP, ICMP, TCP y UDP,
  y el estado del chip (`g_io_base`, los buffers de TX, el `pci::DeviceInfo`)
  vivia en el mismo namespace anonimo que las tablas de sockets TCP. Ahora
  `nic::Nic` es una vtable (`attach`/`bring_up`/`is_up`/`mac_address`/
  `transmit`/`poll_receive`/`get_stats`) que es **todo** lo que el stack sabe del
  hardware: del otro lado no se filtran registros, puertos de I/O ni el layout
  del ring de recepcion. El driver habla hacia arriba por `nic::Events`, con un
  callback de frame recibido y otro de estado para los diagnosticos que solo el
  puede hacer (`TX_FAILED`, `TX_TIMEOUT`, `RX_INVALID`, `BRING_UP_FAILED`); el
  resto de los estados — ARP, ping, READY/IDLE — se los queda el stack.
  `kernel/rtl8139.cpp` se lleva el chip completo (PCI, reset, ring, los 4 slots
  de TX, el handler de INTx ruteada por _PRT/IOAPIC y su fallback a polling) y
  `kernel/nic.cpp` es el dispatcher con el mismo `register_driver`/`bind_best`
  por prioridad que display y audio. `net.cpp` baja de 1647 a 1395 lineas y
  queda como stack L3/L4 puro, sin una sola referencia a `pci::`, `memory::` ni
  a un registro del chip. Nuevo log de boot: `nic: driver 'rtl8139'`. La API
  publica de `net::` no cambia. Verificado con el `net-smoke` que se escribio
  justo antes para esto: mismos resultados que antes de mover una linea —
  `mac=52:54:0:12:34:56`, tres replies con `ttl=255` y `frames tx 0->4 rx 0->4`
  — mas `build.ps1 smoke` y `build.ps1 windowd-smoke` en PASS.

- **El Program Manager ya no lista programas que no estan instalados.** Los
  ports en Haxe (grupo "Native") y Doom se construyen con **builds aparte**, asi
  que en una imagen donde no se instalaron sus entradas se mostraban igual y no
  lanzaban nada — un grupo muerto en el launcher y en el escritorio. Nuevo
  `progman_registry_prune_missing(exists)`: descarta los items cuyo path no se
  puede abrir, y con ellos los grupos que quedan vacios, reapuntando a los
  indices nuevos los items que sobreviven. Se aplica **igual a los defaults
  horneados y a lo que venga de `/disk/progman.ini`**: una entrada que no se
  puede lanzar es ruido venga de donde venga. Es una etapa **aparte del
  parseo** a proposito — parsear sigue siendo una funcion pura del texto, que es
  como lo ejercita el selftest, y el pruning es la politica que toca el disco —
  con el predicado de existencia inyectable justamente para poder testearlo sin
  disco. A diferencia de `progman_registry_load()`, esto si puede dejar el
  registro vacio: si de verdad no hay nada lanzable, mostrar nada es lo honesto.
  Nota de alcance: `windowd_appinfo.c` **no** se toca, porque no es un catalogo
  de accesos directos sino la traduccion path -> nombre/icono/color de ventanas
  ya abiertas y del Task List; filtrarla le sacaria el titulo lindo a una app
  que esta corriendo. Verificado con `build.ps1 progman-smoke`: 12 aserciones
  nuevas sobre la logica de pruning con un predicado falso (descarte, borrado
  del grupo vacio, remapeo de indices, `group_item_at` coherente, predicado
  nulo, idempotencia) mas dos contra el filesystem real que confirman que
  `open()` distingue un binario instalado de uno ausente. El selftest ademas
  ahora reporta que hace el pruning contra el disco real de la imagen
  (`prune dropped=N` y los grupos que sobreviven), que es la parte que ningun
  predicado falso puede cubrir.

### Corregido

- **SCI ruteada dos veces: el boton de power no apagaba la maquina.**
  `acpi::start_sci()` ruteaba la GSI del SCI y despues el glue de uACPI ruteaba la
  MISMA GSI, pisando la entrada de redireccion con un vector nuevo y dejando
  huerfano a `acpi::sci_interrupt` (el handler que hace el shutdown S5). El mismo
  choque pasaba en el camino PIC, donde los dos llamaban `register_irq_handler(9)`
  y el segundo sobreescribia al primero. Ahora una linea de interrupcion admite
  varios dueños: `register_interrupt_handler` **encadena** handlers sobre el vector
  (hasta 4, idempotente por puntero, exigiendo el mismo modo de EOI) en vez de
  reemplazar, y `ioapic::route_gsi` reconoce una GSI ya ruteada como linea
  compartida — reusa su vector y encadena el handler sin reprogramar la RTE, en vez
  de quemar un vector nuevo. Esto tambien cubre el caso, ya anticipado, de dos links
  PCI que resuelven a la misma GSI para INTx. Segundo bloqueo en la misma funcion:
  `uacpi_initialize` apaga **todos** los eventos fijos (`initialize_fixed_events`),
  incluido el `PWRBTN_EN` que `start_sci` acababa de habilitar, asi que el boton no
  generaba SCI aunque el ruteo estuviera bien; se agrego
  `acpi::enable_power_button()` (idempotente, no toca el resto de los eventos) y el
  bringup de uACPI la vuelve a llamar al terminar. Verificado en vivo en VirtualBox
  7.2: `VBoxManage controlvm acpipowerbutton` apaga la VM tanto con I/O APIC on (SCI
  por IOAPIC, RTE 9 estable en el vector 50) como off (SCI por PIC). QEMU:
  `build.ps1 smoke` -> SMOKE PASS, con un vector menos consumido en el pool de GSIs.

- **VirtualBox con I/O APIC activado no terminaba de arrancar: soporte xAPIC
  MMIO en el APIC local.** `initialize_local_apic` solo sabia hablar x2APIC (por
  MSR) y VirtualBox nunca expone x2APIC, asi que el APIC local quedaba
  descartado. Con el I/O APIC activo eso era fatal: el firmware de VBox pone la
  maquina en modo APIC (software-enablea el APIC local y **enmascara LINT0**),
  lo que corta el cable del 8259 al CPU; el fallback del PIT desenmascaraba IRQ0
  en el PIC y la interrupcion se quedaba para siempre en el IRR del 8259
  (`IMR:fe ISR:00 IRR:01`, y la RTE 2 del IOAPIC — el override IRQ0->GSI2 de la
  MADT — enmascarada con `irr=1`). Sin ticks el scheduler nunca preemptaba y el
  boot se congelaba en el splash al 100%, con el CPU en `halt_once()`. En el
  mismo escenario, todo lo ruteado por el IOAPIC (PS/2, SCI) hacia EOI contra un
  APIC que el kernel creia inexistente — `local_apic_eoi()` era un no-op — asi
  que el primer scancode dejaba el vector 52 clavado en el ISR (`PPR=0x30`) y
  mataba todos los vectores 48-63. Ahora `initialize_local_apic` mapea la ventana
  MMIO que apunta `IA32_APIC_BASE` (una pagina uncacheable via
  `vm::map_kernel_mmio`) cuando no hay x2APIC, y `read_local_apic`/
  `write_local_apic` despachan entre MSR y MMIO; los helpers del timer del APIC
  dejan de exigir x2APIC. Ademas se repone LINT0 como ExtINT desenmascarado al
  software-enablear el APIC (virtual wire), que es lo que mantiene vivo el camino
  PIC legacy — necesario en VirtualBox **sin** I/O APIC, donde no hay MADT y PS/2
  sigue yendo por el 8259. `ioapic::initialize` ahora se niega a correr si no hay
  APIC local operativo (en vez de programar entregas que nadie puede EOI-ear), y
  `route_gsi` usa el APIC id real como destino en vez de 0 fijo. Nuevo log de
  boot: `cpu: APIC local en modo xAPIC|x2APIC (id N)`. Verificado en vivo en
  VirtualBox 7.2 en las dos configuraciones (I/O APIC on y off): escritorio
  completo, timer del APIC local periodico en el vector 48, Ctrl+Esc abre el Task
  List, ISR/IRR limpios y `PPR=0`. QEMU sigue por el camino x2APIC sin cambios
  (`build.ps1 smoke` -> SMOKE PASS).

## [0.3.3] - 2026-07-09

### Agregado

- **Audio en VirtualBox: driver AC'97 + HAL de audio con backends.** El audio
  solo funcionaba con `virtio-sound-pci` (QEMU); VirtualBox no emula ese chip,
  asi que `/dev/audio0` no se registraba y todo quedaba mudo (Doom incluido). Se
  refactorizo el subsistema a un HAL espejo del de display (`display::Backend`):
  un dispatcher `audio::` con vtable `Backend` (`ready`/`get_info`/`configure`/
  `submit_period`/`stop`), un `audio_device.cpp` agnostico que registra
  `/dev/audio0` y concentra la logica comun (owner-pid de un solo escritor,
  validacion de usuario, troceado en periodos, ioctl `GET_INFO`), y dos backends:
  `virtio_sound` (refactorizado, sin cambios de comportamiento) y el nuevo
  `ac97`. El driver **AC'97** (`kernel/ac97.cpp`) maneja el controlador Intel
  ICH (clase PCI `0x04`/subclase `0x01`, el chip que VirtualBox y QEMU emulan por
  defecto): saca al codec del cold-reset, desmutea el mixer, y reproduce por
  bus-master DMA con un Buffer Descriptor List de 32 entradas en memoria
  fisicamente contigua (`memory::allocate_contiguous_pages`). Es **polling puro
  sondeando CIV** (sin IRQ), lo que esquiva el problema conocido de INTx legacy
  que no llega en VirtualBox. El formato fijo del ABI (S16 estereo 48 kHz) es el
  rate nativo del DAC de AC'97, asi que no hay VRA ni resampling. `kernel_main`
  elige backend igual que en display: virtio-sound si el probe PCI lo encontro,
  si no AC'97 (fallback de VirtualBox). ABI: `savanxp_audio_backend` suma
  `SAVANXP_AUDIO_BACKEND_AC97`. Nuevo target `.\build.ps1 ac97-smoke` que corre
  el smoke de `/dev/audio0` forzando `-device AC97` sin virtio-sound, para
  ejercitar el camino de fallback headless. Verificado: el smoke normal (virtio)
  sigue en verde sin regresion; el `ac97-smoke` detecta el controlador
  (`ac97: ready ... nam=0x6000 nabm=0x6400`), registra `/dev/audio0` y pasa; y
  bajando `kMaxInFlight` a 2 se confirmo que `submit_period` bloquea hasta que el
  hardware consume (CIV avanza), probando que el motor DMA reproduce de verdad y
  no solo acepta el enqueue. Confirmado con sonido en VirtualBox real y en QEMU.
- **`virtio-sound`: fin del enmudecimiento silencioso.** Si el dispositivo
  responde pero ningun stream de salida ofrece el formato fijo del ABI, antes
  `/dev/audio0` no se registraba sin dejar traza (indistinguible de "no hay
  hardware"); ahora se registra el fallo por consola.
- **LiveCD: `/disk` autocontenido en la ISO via ramdisk escribible.** La imagen
  de disco (`disk.img`, SVFS2) ahora viaja *dentro* de la ISO como un segundo
  modulo de Limine (`boot/limine.conf`), en vez de depender de un disco IDE
  adjuntado solo en el `run` de QEMU. El kernel la expone como un block device
  respaldado en memoria: `block::register_ramdisk` agrega un `Device` de tipo
  `Kind::memory` (read/write ruteados a `memcpy`) despues de sondear los ATA,
  asi que un disco IDE persistente (dev) mantiene prioridad y la ISO pura monta
  el ramdisk. `kernel_main` lo registra entre `block::initialize()` y
  `svfs::initialize()` (que no cambia: ya monta el primer SVFS2 valido). El
  handoff del boot pasa `disk_image_address/size` en `BootInfo`, y
  `arch/x86_64/entry.cpp` clasifica los modulos por sufijo de ruta
  (`initramfs.cpio` vs `disk.img`) en lugar de asumir `modules[0]`. El ramdisk
  es **escribible-efimero, in-place** sobre la RAM del modulo (mapeada RW por el
  HHDM de Limine, cuyo CR3 reutiliza el kernel): las apps que crean
  directorios/archivos en `/disk` funcionan y los cambios se pierden al
  reiniciar, la semantica correcta de un LiveCD. `build.ps1` stagea `disk.img`
  en la ISO junto a `kernel.elf`/`initramfs.cpio`. Resultado: la ISO arranca por
  si sola con `/disk` montado en VirtualBox y hardware real, resolviendo que las
  apps de `/disk` (Doom, etc.) no aparecieran al bootear la ISO sin el disco de
  dev. Verificado en vivo: boot UEFI sin disco IDE -> `/disk mounted`; el `smoke`
  (mkdir/write/cp/mv/rm en `/disk`) corrido sin IDE pasa todas las escrituras
  contra el ramdisk; y confirmado en VirtualBox arrancando desde la ISO. La
  instalacion persistente a disco queda como fase futura.
- **Doom con Freedoom (IWAD libre) en el LiveCD.** `sdk/doomgeneric/build.ps1`
  hornea `freedoom1.wad` (IWAD 100% libre que el motor ya reconoce en
  `d_iwad.c`) por defecto en `/disk/games/doom/`, en vez del `doom1.wad`
  shareware, para que la ISO distribuible lleve Doom jugable sin contenido
  propietario. El motor igual detecta `doom1.wad`/`doom.wad` si el usuario los
  aporta. El WAD (~28.8 MB) vive dentro de la `disk.img` de tamano fijo, sin
  costo de RAM extra sobre el modulo.
- **Capa IOAPIC/MADT y ruteo de IRQs por GSI.** Nuevo subsistema que parsea la
  MADT (descubre IOAPICs y sus Interrupt Source Overrides), programa las
  redirection entries y rutea GSIs hacia vectores de la IDT con entrega por el
  Local APIC (`ioapic::route_gsi` / `route_legacy_irq`), inicializado en
  `kernel_main` tras el Local APIC y la ACPI. Reserva el pool de vectores
  50-63 en `cpu_init` (trampolines `vector_NN` + gates IDT) para las GSIs
  ruteadas. PS/2 (IRQ1/IRQ12) migra a este ruteo cuando el IOAPIC esta
  disponible, con fallback al PIC legacy si no hay MADT/IOAPIC (protege el
  arranque de VirtualBox). Verificado en vivo en QEMU q35: MADT con 1 IOAPIC,
  5 overrides y 24 GSIs; teclado responde tecleando de verdad y el serial
  queda limpio tras el handoff (sin overflow de cola ni interrupciones
  espurias). Es el prerrequisito para rutear la SCI de ACPI y el INTx legacy
  resuelto por `_PRT`.
- **ACPI: SCI ruteada por IOAPIC + boton de power.** `acpi::start_sci()`
  habilita el modo ACPI (`SCI_EN` via `SMI_CMD`), enmascara todas las GPE (sin
  interprete AML, evita tormentas en la SCI level-triggered), habilita el
  evento fijo PWRBTN y rutea la SCI con `ioapic::route_gsi(sci_int,
  active_low, level, ...)`, con fallback al PIC cuando la GSI entra en 0..15 y
  no hay IOAPIC. El handler `sci_interrupt` lee PM1a/b_STS: ante PWRBTN hace
  ack (write-1-to-clear) y dispara `acpi::shutdown()` (S5, mismo camino que
  `/dev/power`); limpia cualquier otro bit fijo para no re-disparar la SCI.
  Parsea de la FADT los bloques de evento PM1, los bloques GPE, SMI_CMD y
  sci_int. Verificado en vivo en QEMU q35: SCI ruteada en GSI 9 sin regresion
  ni tormenta de interrupciones; Machine -> Power Down apaga la VM por S5 via
  el handler. El apagado es inmediato (sin cierre graceful de userland);
  senializar a init queda como mejora futura.
- **uACPI vendorizado e integrado: interprete de AML real.** Copia horneada de
  uACPI v6.0.0 (github.com/uACPI/uACPI, commit upstream `9c9b26d`) bajo
  `vendor/uacpi/`, sin submodulo (misma convencion que busybox), para
  reemplazar progresivamente el ACPI hand-rolled (que solo escaneaba `_S5_`
  sin interpretar AML). Se compila como C11 (no C++, usa conversion implicita
  `void*`->`T*`): `tools/Ninja.ps1` gana una 3a variable de flags
  `uacpiflags`; `build.ps1` aporta `Get-UacpiFlags`
  (`-std=gnu11 -DUACPI_USE_BUILTIN_STRING`, misma ABI freestanding/
  `-mcmodel=kernel` que el kernel) y `Get-UacpiCompileEdges`. Glue en
  `kernel/uacpi_glue.cpp`: capa `uacpi_kernel_*` mapeada a
  heap/vmm/pci/timer/ioapic (mutex/spinlock/event triviales, monocore);
  fuente de tiempo = TSC calibrado contra el PIT canal 2 por polling, sin IRQ
  (`timer::ticks()` no sirve porque en el bringup las interrupciones estan
  apagadas); el SCI del glue rutea por `ioapic::route_gsi`. Se agrega
  `pci::write_config_u8` (antes solo habia u16/u32). `uacpi_glue::bringup()`
  corre `uacpi_initialize` + `uacpi_namespace_load` desde `kernel_main` (tras
  `acpi::start_sci`), conviviendo con la ACPI hand-rolled;
  `uacpi_namespace_initialize()` (corre `_INI`/`_STA`) queda diferida a la
  etapa de eventos porque requiere el ACPI Global Lock con el subsistema
  GPE/SCI de uACPI activo e interrupciones habilitadas — el namespace ya
  cargado alcanza para `_CRS`/`_PRT`. Verificado en vivo (desktop-smoke
  PASS): uACPI inicializa, encuentra las tablas e interpreta el DSDT
  ("successfully loaded 1 AML blob, 1707 ops").
- **Ruteo de INTx via `_PRT` de uACPI, cerrado de punta a punta.**
  `uacpi_glue::dump_pci_routing()` recorre los root bridges PCI/PCIe
  (`PNP0A03`/`PNP0A08`), obtiene el `_PRT` de cada uno y resuelve cada PCI
  link device a su GSI real evaluando el `_CRS` del link
  (`uacpi_get_current_resources` + `uacpi_for_each_resource`, primer
  descriptor IRQ/Extended IRQ). En q35 todas las entradas del `_PRT` usan
  link devices (nunca GSI directa); verificado en vivo: `LNKA/B/E/F` ->
  GSI 10, `LNKC/D/G/H` -> GSI 11 (level). `uacpi_glue::route_pci_intx(bus,
  dev, func, handler)` cierra el camino real: lee el Interrupt Pin de la
  config PCI (`0x3D`), busca la entrada `_PRT` del root (device+pin), resuelve
  el link a GSI via `_CRS` y programa el IOAPIC con la polaridad/trigger que
  declara el firmware. El `rtl8139` pasa de polling a interrupt-driven sobre
  este camino: `net::initialize` rutea su INTx una vez (registra
  `net_irq_handler`); `bring_up` habilita el IMR (`ROK|TOK|RER|TER`) solo si
  el ruteo tuvo exito, si no `IMR=0` y sigue el polling como fallback;
  `net_irq_handler` sirve el device (`poll_receive` drena RX y limpia el ISR,
  desasertando la linea level-triggered, sin storm); `poll_receive` se
  envuelve en un `cli` local (`irq_save`/`restore`) porque corre tanto desde
  el handler de IRQ como desde el main (timer/service), serializando el
  acceso al ring de RX en monocore. Verificado en vivo en QEMU q35: "INTx dev
  1 INTA -> GSI 10 ruteado (vector 53)" y "net: primer INTx recibido via
  IOAPIC" al arranque, una sola vez (sin storm), escritorio sano. INTx legacy
  ahora llega en q35+APIC, que era el bottleneck que habia forzado usar
  MSI-X para virtio-gpu.
- **Subsistema nativo — Fase 2: ABI nativo v1 + runtime real.** El contrato
  kernel<->userland vive en `subsystems/native/sdk/include/savanxp_native_abi.h`
  (unica fuente, incluida por ambos lados): espacio de syscalls particionado
  (`< 0x1000` baseline transitorio delegado en posix, `>= 0x1000` syscalls
  propias que para un proceso posix no existen), version de ABI y primeras dos
  syscalls nativas: `SXN_SYS_INFO` (identidad + version, con handshake
  obligatorio del runtime al arrancar; si no coincide aborta con exit 132) y
  `SXN_SYS_LOG` (log al kernel con prefijo `native[pid]:`). El runtime de
  userland suma heap propio (`sxn_alloc/realloc/free`, free-list sobre arena BSS
  de 4 MiB), builtins de memoria, `operator new/delete` y un mini `<memory>`
  freestanding (shared_ptr/make_shared no-atomico en una asignacion), con lo
  cual las clases Haxe con semantica por defecto y `@:valueType` ya compilan y
  corren sin libstdc++. Verificado en QEMU: handshake `abi verificado, version=1`
  y clases Haxe sobre el heap nativo con resultados correctos.
- **Subsistema nativo — override `_std`: String y Array reales de Haxe.** El
  `_std` de reflaxe.CPP se expone como overrides `*.cross.hx` (generados por el
  build en `build/native/std-cross/`), el mecanismo oficial de Haxe para
  overrides por plataforma: aplican solo al target de generacion y no
  envenenan el contexto macro/eval (que como `.hx` planos explotaba con errores
  cripticos). El mini std C++ freestanding del SDK crece con `<string>`
  (std::string + literales "..."s + to_string), `<deque>` (respaldo del
  Array<T>), `<initializer_list>`, `<algorithm>`, `<cctype>`, `<new>` y un
  nucleo compartido `__sxn_core`. Dos fixes al codegen sin parchear las libs
  pineadas: `haxe-std-fixes/Math.hx` (shadow con el overload isFinite ambiguo
  en Haxe 4 colapsado a uno) y el preprocesador custom `UniqueLocalNames`
  (registrado por `SxnCompilerInit` via `ExpressionPreprocessor.Custom`), que
  hace unicos los locals por funcion porque el codegen aplana bloques hermanos
  y los contadores `_g` de dos for-in colisionaban en el mismo scope de C++.
  Verificado en QEMU: concat + toUpperCase (`HAXE NATIVO EN SAVANXP`), array
  con push e iteracion (`suma del array=100`) y SMOKE PASS sin regresiones.
- **Subsistema nativo — ABI gfx + hello GUI en Haxe.** Nuevo bloque de
  syscalls de graficos en el ABI nativo (`0x1010`): SXN_SYS_GFX_INFO /
  GFX_ACQUIRE / GFX_RELEASE / GFX_PRESENT. El display es parte del ABI de
  primera clase (sin `/dev/gpu0` ni ioctls); kernel-side comparte los internals
  `display::*` y la sesion exclusiva por pid con los ioctls GPU de posix
  (incluida la liberacion automatica al morir el proceso). El runtime suma los
  wrappers `sxn_gfx_*` y Main.hx un hello GUI real: la clase `Lienzo` dibuja un
  degrade con rectangulo sobre un `Array<Int>` de Haxe (contiguo por garantia
  del mini `<deque>`) y lo presenta. De paso, dos fixes de codegen importantes:
  se excluye el pase `RemovePureExpressions` de reflaxe (eliminaba los `if`
  cuyos cuerpos son solo inyecciones `__cpp__` — codigo incorrecto que
  compilaba) y se compila con `--no-opt` (el analizador de Haxe const-foldea
  condiciones dependientes de inyecciones). Documentado: Float de Haxe todavia
  no funciona en freestanding (sin soft-float intrinsics). Verificado en QEMU:
  `gfx: present=0`, `gfx: release=0` y `gputest --smoke` adquiriendo la sesion
  despues sin fugas; SMOKE PASS.
- **Subsistema nativo — protocolo cliente del compositor (apps ventaneadas).**
  Nueva capa `sxn_gui_*` en el runtime nativo (`sdk/runtime/sx_gui.c` +
  `savanxp_native_gui.h`): habla el contrato de superficie v3 del compositor
  sobre los fds 3..9 que el shell instala antes del exec (mapeo de la seccion
  compartida con validacion del header SXGF, submit de batches de dirty rects
  con secuencias submit/composed y eventos de retire/shutdown, input por pipe
  con resize sintetizado) — todo sobre syscalls del baseline, sin kernel nuevo.
  Primera app ventaneada nativa: `nativegui` (subsystems/native/haxe-gui,
  `build.ps1 -Name nativegui -Source haxe-gui`), que dibuja, anima con regiones
  sucias y procesa teclado. Verificacion headless con `test/guihost.c`, un
  harness POSIX que interpreta el rol del compositor (seccion + eventos +
  composicion + tecla) y valida secuencias, rects y pixeles; de paso es el
  primer test del cambio de subsistema via exec (fork posix -> exec ELF
  nativo). En QEMU: `gui: frames compuestos=4`, tecla ENTER recibida,
  `NATIVEGUI HOST PASS` y SMOKE PASS sin regresiones. Lanzada desde el
  escritorio real, `nativegui` corre como ventana normal.
- **Header esperable generico en el Object Manager (`object::Header`).**
  `Header` gana `waitable`/`manual_reset`/`signal_count`, migrando el estado
  de senializacion que antes vivia duplicado por tipo (`EventObject::signaled`,
  `TimerObject::signaled`) a la base comun. `can_satisfy_wait`/
  `try_acquire_wait` pasan de un `if (event) ... else if (timer) ...` a logica
  generica sobre `Header`, y `object_is_waitable` (`process.cpp`) pasa de una
  lista cerrada de tipos (`event || timer`) a un solo campo booleano: un tipo
  esperable nuevo no requiere tocar el despachador de esperas. Cambio
  contenido — ningun caller fuera de `object.cpp` tocaba `.signaled`/
  `.manual_reset`/`.armed` directamente, todo pasaba por la API publica
  (`create_event`, `set_event`, `poll_timers`, etc.), incluido `virtio_gpu.cpp`.
  Verificado: kernel compila limpio y `desktop-smoke` sigue en PASS (el
  compositor usa `EventObject` via `virtio_gpu.cpp`).
- **Semaforo real (`SAVANXP_SYS_SEMAPHORE_CREATE`/`_RELEASE`).** Primer uso
  del header esperable generico: `object::SemaphoreObject` (pool de 64) con
  `create_semaphore(initial, max)` y `release_semaphore` (satura en
  `max_count`, rechaza sin modificar nada el release que lo excederia). El
  despertar de esperas (`wake_waiters_for_object`) ya soportaba semantica de
  contador sin cambios porque opera sobre `Header::signal_count` de forma
  generica. Syscalls 53/54 con handlers en `process.cpp` (mismo patron que
  event/timer: `allocate_fd` + `access_query|modify|synchronize`), wrappers
  `semaphore_create`/`semaphore_release` en el SDK, y `semaphoretest`
  (creacion invalida, conteo hasta el tope, despertar cruzado entre procesos
  via `fork`) enganchado en la suite `smoke`. `Type::semaphore` existia en el
  enum desde antes sin implementacion (dead code). Verificado:
  `build.ps1 smoke` -> SMOKE PASS con el nuevo test incluido.

### Corregido

- **virtio-sound: reproduccion TX async multi-buffer (sin espera bloqueante).**
  El camino TX enviaba UN periodo y hacia spin esperando que el device lo
  consumiera (`wait_for_used_element`) antes de aceptar el siguiente, con un solo
  buffer compartido. Ese spin corre con interrupciones deshabilitadas (los
  syscalls corren asi), con el mismo riesgo de congelar el reloj del guest que se
  corrigio en AC'97. Ahora la cola TX usa un anillo de `kTxSlots` periodos, cada
  uno con su cadena de 3 descriptores (header/data/status): `submit_period`
  encola sin esperar y reclama los completados por el used ring; si el anillo
  esta lleno descarta en vez de bloquear. Se agrego un colchon inicial de
  silencio (como AC'97) como seguro, aunque en QEMU virtio-sound completa cada TX
  al instante y bufferea del lado del host, asi que el colchon/underrun del
  modelo de AC'97 no aplica aca (reinyectar silencio en ese caso inundaria el
  stream — por eso no se hace). Verificado headless con `.\build.ps1
  virtio-stream` (captura WAV del patron de feed tipo-Doom): tono continuo, 0
  gaps; `virtio-count` reporta 0 periodos descartados; `smoke` sin regresion. A
  diferencia de AC'97, la captura WAV de virtio bajo TCG SI es fiel (no hay
  emulacion de DMA continua que ralentice el guest).
- **AC'97: modelo de reproduccion sin bloqueo + colchon + sin IOC.** Tres
  correcciones al camino de reproduccion, sobre el fix de subalimentacion de
  Doom (abajo), apuntando al audio entrecortado en VirtualBox:
  - *No bloqueante:* `submit_period` ya no hace spin esperando una ranura libre
    cuando el ring esta lleno; descarta el periodo y sigue. Ese spin corria con
    interrupciones deshabilitadas (los syscalls corren con IRQ off), congelando
    el timer y con el el reloj del guest; como Doom se guia por ese reloj para
    dosificar el audio *y* para su logica, congelarlo lo desincronizaba.
  - *Sin IOC:* las entradas del BDL ya no llevan el bit interrupt-on-completion.
    El driver reproduce por polling de CIV y no registra IRQ de AC'97, asi que
    con IOC la reproduccion continua disparaba una interrupcion por periodo que
    nadie atendia.
  - *Colchon:* al preparar el stream se precargan `kPrimePeriods` periodos de
    silencio y se reinyectan si el ring se vacia, para absorber el jitter del
    productor sin quedar al borde del underrun.
  - Verificacion headless nueva: `audiotest --stream` reproduce el patron de
    alimentacion por-tic de Doom; `.\build.ps1 ac97-count` lo corre con
    `audiodev none` y reporta el contador de underruns del driver (0 con
    colchon). `.\build.ps1 ac97-stream` graba a WAV (`tools/audio/wavgaps.py`
    detecta gaps), util solo con aceleracion por hardware: bajo TCG el backend
    wav marca el ritmo a tiempo-real del host y el guest no lo alcanza.
- **Audio "robotizado"/trabado en Doom (subalimentacion del device).**
  `DG_Sound_Update` (glue de audio de doomgeneric) mezclaba `frames_to_mix`
  frames avanzando la posicion de todos los canales, pero escribia solo ese
  total redondeado *hacia abajo* al periodo, descartando el resto. A ~35 Hz (un
  tic de Doom ~28.5 ms > un periodo de 21.3 ms) eso alimentaba el device a
  ~0.75x del ritmo de reproduccion (underrun constante => cortes) y ademas
  adelantaba los efectos ~1.34x (los frames descartados ya habian avanzado los
  canales). Estaba en la capa comun del glue, por eso sonaba igual en virtio y
  AC97; el tono de `audiotest` (una sola escritura) no se afectaba. Fix: escribir
  todos los frames mezclados (el kernel ya trocea en periodos internamente).
- **Residuos visuales del cursor sobre elementos del compositor.**
  `sx_painter_draw_frame` (SDK `gfx2d.c`) intersectaba el rect con el clip del
  painter y luego dibujaba el marco del rect *intersectado*. Cuando el
  compositor repinta un elemento en fragmentos (los sub-rects sucios del
  cursor por software), cada fragmento recibia su propio borde trazado alrededor
  del contorno del fragmento, dejando lineas del color del marco (p. ej.
  `rgb(46,50,56)`) dentro del elemento: el clasico "rastro" con forma de la
  huella del cursor sobre el rectangulo de seleccion de iconos, los botones de
  ventana, la pantalla de bienvenida y el dialogo de energia. Una invalidacion
  de pantalla completa (abrir el menu inicio) lo limpiaba porque el elemento se
  repintaba en un solo rect. El fix dibuja el marco del rect *original* como
  cuatro tiras de borde, cada una recortada por `sx_painter_fill_rect` (que si
  recorta correctamente), pintando solo los pixeles de borde que existen de
  verdad. Las superficies de apps (sxgui) eran inmunes porque se blitean
  completas cada frame, sin `draw_frame` fragmentado. Nuevo test de regresion
  headless `build.ps1 cursor-repro` (`desktop --cursor-repro`): mueve el cursor
  sobre un dialogo estatico con solo damage del cursor y verifica que el
  backbuffer en la posicion vieja vuelve al baseline (0 px de residuo).
  Verificado: cursor-repro PASS, desktop-smoke PASS, smoke PASS.
- **`build.ps1` bifurcaba silenciosamente a PowerShell 5.1 para generar
  assets.** `Generate-CursorAsset`/`Generate-DesktopIconAssets` invocaban
  `& powershell -ExecutionPolicy Bypass -File ...` (3 puntos) para correr
  `GenerateCursorAsset.ps1`/`GenerateDesktopIconAssets.ps1`/
  `GenerateDesktopSourceArt.ps1`, que usan `System.Drawing`. `powershell` sin
  extension resuelve siempre a Windows PowerShell 5.1 sin importar el motor
  que arranco `build.ps1` de afuera: correr el build entero con `pwsh` (7) daba
  una falsa sensacion de portabilidad, porque el tramo que usa GDI+ nunca se
  ejecutaba realmente bajo pwsh 7, solo el script raiz. Se cambio a invocar los
  scripts directamente (`& $scriptPath ...`, in-process, sin subproceso ni
  `$LASTEXITCODE`; cada script ya propaga sus propios errores via
  `$ErrorActionPreference = "Stop"`). Es el primer paso de evaluar portar el
  build a WSL2/Ubuntu: valida antes que nada que el pipeline corra de verdad
  bajo pwsh 7 en el mismo Windows, sin mezclar el salto de version de
  PowerShell con el salto de sistema operativo. Verificado: `build`/`iso`/`run`
  desde cero bajo `pwsh` generan los mismos assets y el kernel arranca igual
  hasta el handoff.
- **Rutas de `Join-Path` con `\` embebido, incompatibles fuera de Windows.**
  Varias llamadas pasaban un segundo componente con `\\` literal dentro del
  string (p. ej. `"EFI\\BOOT"`, `"runtime\\libc.c"`). En Windows funcionaba
  por casualidad: .NET colapsa `\\`, `\` y `/` como el mismo separador de
  directorio al tocar el filesystem real. En Linux `\` no tiene significado de
  separador y queda como caracter literal del nombre de archivo, rompiendo la
  jerarquia esperada (`EFI\BOOT` en vez de `EFI/BOOT`). Normalizado a `/` en
  `build.ps1` y `tools/UserAppCommon.ps1` (~18 ocurrencias), que es separador
  valido en ambos sistemas. Fuera de alcance a proposito: `ConvertTo-CygwinPath`
  (traduccion especifica de rutas `C:\...` para el xorriso de Cygwin horneado)
  y el contenido de `startup.nsh` (ruta de shell UEFI que interpreta el
  firmware, no el host). Verificado: build/iso/run desde cero bajo `pwsh`,
  mismos artefactos (`EFI/BOOT/BOOTX64.EFI`, `boot/limine/*`,
  `rootfs/bin/busybox`) y mismo arranque en QEMU.

### Cambiado

- **Compilacion de kernel+userland via Ninja.** `build.ps1` compilaba
  ~250-300 fuentes de forma secuencial y sin incremental (`Compile-Object`
  invocaba `clang++` archivo por archivo). Se reemplaza esa fase por Ninja
  (paralelo + tracking de dependencias de headers via `-MMD`), pineado en el
  toolchain igual que LLVM/QEMU/xorriso. El link, la imagen SVFS2, la ISO y
  QEMU siguen manejados por `build.ps1` sin cambios.

### Corregido

- **Pulido de `fb_gpu` y `virtio-gpu` para madurar la base visual.**
  `present_region` de `fb_gpu` leia el origen desde la fila 0 en vez del
  offset (x,y) de la superficie completa (contrato del ioctl, igual que
  virtio y console); en VirtualBox pintaba pixeles equivocados en presents
  parciales. `GPU_IOC_GET_STATS` de `fb_gpu` ahora reporta
  `present_enqueued`/`completed` reales en vez de un struct en cero.
  `refresh_scanouts` de `virtio-gpu` ya no pisa un flip fullscreen (superficie
  importada como scanout) al llegar un evento de display: re-emite
  `SET_SCANOUT` para el scanout activo y solo cae a la primaria como
  fallback. El header `used` de la cola de cursor se lee volatile, como el de
  la cola de control. `notify_off_multiplier == 0` (valido segun la spec:
  todas las colas comparten la direccion de notify) ya no hace que se salte
  el notify. `GPU_IOC_REFRESH_SCANOUTS` exige la sesion grafica como el resto
  de los ioctls que mutan estado del dispositivo. El soak de `gputest` suma
  un subtest que ejercita `SET_MODE` en runtime (640x400 y vuelta al nativo),
  cubriendo el camino `RESOURCE_UNREF` de recreacion de recursos primarios.
  Validado: desktop-smoke (virtio) PASS, gpu-soak PASS con el subtest de modo
  (0 timeouts, sin degraded/recovery), y desktop-selftest PASS sobre VGA
  estandar (backend fb_gpu, incluye reconexion de compositord).
- `savanxp_mode_bits` (SDK) pierde el tipo subyacente fijo del enum: evitaba
  el warning `-Wfixed-enum-extension` (extension no estandar de Clang en C)
  repetido en cada TU que incluye `syscall.h`. Los valores caben en un int
  normal y el tipo no se usaba en ninguna firma; sin cambio funcional.

### Agregado

- **`build.ps1 run`/`debug`: soporte `-Accel whpx` para acelerar QEMU con
  Hyper-V.** Por defecto se sigue usando TCG (emulacion por software); en
  maquinas con Hyper-V activo, `-Accel whpx` cambia a Windows Hypervisor
  Platform. `-cpu max`/`-cpu host` bajo whpx hacen crashear a OVMF con un
  `#GP` en `PlatformPei` apenas arranca (fase PEI, antes de DXE): WHPX no
  puede respaldar para el guest ciertas features de CPU muy nuevas (conflicto
  APX/MPX en CPUID leaf 7) que esos modelos exponen, y la deteccion temprana
  de features de OVMF lo hace fallar. `-Accel whpx` fuerza `-cpu qemu64` en
  su lugar, que evita el problema; el kernel/userland de SavanXP no dependen
  de AVX/RDRAND/XSAVE directamente asi que no pierde nada util. `smoke`/
  `desktop-smoke`/soak (`Run-AutomationQemu`) se dejan a proposito
  hardcodeados en TCG, para no meterle no-determinismo a los tests
  automatizados. Verificado: boot end-to-end hasta `handoff: starting
  /bin/init`, con el AML de uACPI cargando ~10x mas rapido que bajo TCG.

### Corregido

- **`virtio-gpu`: `SET_SCANOUT` colgado para siempre bajo WHPX (HLT con
  interrupciones deshabilitadas).** Con `-Accel whpx` el boot llegaba hasta
  `virtio-gpu: primary backing attached` y se quedaba trabado ahi (pantalla
  de "Preparando display" sin avanzar nunca). Causa: durante el boot
  temprano el kernel corre con interrupciones globalmente deshabilitadas
  (`IF=0`); cuando `SET_SCANOUT` tardaba mas que el busy-spin inicial de
  `wait_for_command_slot`, el segundo tier de espera caia a
  `timer::wait_ticks()` -> `halt_once()` (un `HLT` desnudo, sin `sti`
  previo) esperando la interrupcion del timer. Un `HLT` con `IF=0` en
  hardware real (y en WHPX, que es fiel al hardware) solo se puede despertar
  con NMI, nunca con una IRQ enmascarable como la del timer -> cuelgue
  permanente. TCG es mas laxo con ese caso puntual y despierta el guest
  igual, por eso el bug nunca se habia manifestado antes de probar whpx.
  Confirmado con el monitor HMP de QEMU: dos lecturas de `info registers`
  con varios segundos de diferencia devolvieron el estado del vCPU identico
  bit a bit (`HLT=1`, `RFLAGS=0x97` sin el bit IF). Fix: nueva
  `timer::monotonic_ns()` (reloj por TSC, reutiliza la calibracion que
  uACPI ya hacia para su propio `stall`/`sleep` con `IF=0`) reemplaza a
  `timer::ticks()` en el wait de `virtio-gpu`, con busy-spin acotado en vez
  de `HLT`; el TSC avanza sin importar el estado de interrupciones. Aplica
  siempre (no solo bajo whpx) sin romper nada: `smoke` y `desktop-smoke`
  siguen en verde bajo TCG. Confirmado en la maquina real del usuario:
  `.\build.ps1 run -Accel whpx` arranca completo de punta a punta.

## [0.3.2] - 2026-07-03

### Agregado

- **sxgui completo: toolkit de widgets estilo Win9x.** La libreria del SDK pasa
  de 5 controles basicos a un toolkit completo, manteniendo el modelo
  retained-mode allocation-free (la app posee el array plano de widgets, los
  buffers de texto y las tablas de items; el toolkit solo pinta y despacha
  input):
  - Recorrido de foco con **Tab/Shift+Tab** (el estado de Shift se trackea via
    KEY_DOWN/KEY_UP; antes `sxgui_handle_key` descartaba todo KEY_UP).
  - **Textfield con caret real**: navegacion con flechas/Home/End, insercion y
    borrado (Backspace/Delete) en la posicion del caret, click posiciona el
    caret midiendo prefijos, y scroll horizontal para mantenerlo visible.
  - **Scrollbar** como widget (flechas, track paginable, thumb proporcional
    dragueable; horizontal via `SXGUI_FLAG_HSCROLL`) y **scroll en el listbox**
    con columna embebida que reutiliza la misma maquinaria, seleccion
    consciente del scroll y teclas Home/End/PageUp/PageDown con
    ensure-visible. La captura de puntero vive en el contexto: los drags van
    solo al widget que recibio el press.
  - **Double-click y motivo de accion**: `widget->action` distingue
    CLICK/CHANGE/ACTIVATE en los callbacks; el listbox dispara ACTIVATE por
    doble click (umbral 450 ms) o Enter.
  - **Radio buttons** mutuamente excluyentes por group id (marcar uno limpia el
    resto del grupo en el array, sin allocations).
  - **Combobox** con dropdown overlay dentro del backbuffer propio (sin
    ventanas hijas), clampado a la superficie y abriendo hacia arriba si no
    entra; mientras esta abierto posee puntero y teclado, y ESC/click afuera lo
    cierran consumiendo el evento.
  - **Barra de menu y menus desplegables** con tablas caller-owned
    (separadores, items disabled/checked), hover que cambia de menu abierto,
    navegacion por teclado y `on_command(id)`.
  - **Dialogos modales**: segundo array de widgets con rects relativos pintado
    como overlay centrado; captura todo el input re-enrutando el dispatch con
    coordenadas trasladadas, Tab cicla adentro, ESC cierra con result 0. Sin
    loop anidado: es una maquina de estados dentro del mismo main loop.
  - **Groupbox** (frame etched con caption), **progress bar**, labels con
    panel hundido (`SXGUI_FLAG_SUNKEN`) y **textview** multilinea read-only
    con el scroll del listbox.
- **App frame `sxgui_app`** (`runtime/sxgui_app.c`): encapsula la sesion gfx y
  el main loop que toda app de widgets repetia (poll de teclado/puntero,
  RESIZED con `sxgui_context_retarget`, repaint gateado, present y throttle de
  16 ms), con hooks opcionales `on_key`/`on_paint`/`on_resize`. ESC cierra la
  app salvo que el toolkit lo consuma antes (menu/popup/dialogo abierto).
- `widgetsdemo` crece como galeria de referencia de todo el toolkit (lista
  larga con scroll, radios, combobox, menubar File/Edit/Help con Exit
  funcional, dialogo About modal).

### Cambiado

- **`aboutapp` y `filesapp` portadas a sxgui.** aboutapp queda declarativa
  (groupboxes + labels + botones Refresh/Close, F5 via hook). filesapp
  conserva toda la logica de filesystem (opendir/stat/sort/preview/launch)
  pero delega en el toolkit la lista con scroll, el doble click, el preview
  (textview), la barra de menu (File: Refresh/Go up/Exit; Help: About modal)
  y el statusbar. Ambas pierden su backbuffer estatico de 8 MiB y el loop de
  eventos manual.
- **La arena de malloc del SDK baja de 48 MiB a 8 MiB por defecto.** El
  `g_heap` estatico de `posix.c` vive en la BSS y el kernel mapea la BSS
  entera al exec, asi que cada app del sistema costaba ~50 MiB residentes
  (con 3 abiertas se agotaba la memoria fisica). El default es ahora 8 MiB
  (`#ifndef SX_HEAP_SIZE`); el build externo del SDK (`UserAppCommon.ps1`)
  conserva los 48 MiB via `-DSX_HEAP_SIZE` para apps pesadas como Doom.

### Corregido

- **Fuga de memoria fisica en el fork por paginas de section views**
  (`vm::clone_address_space`): se alocaba y copiaba una pagina por cada pagina
  de usuario presente, y recien despues se descartaban con `continue` las que
  pertenecian a section views (que se re-mapean compartidas o clonadas por
  otro camino), sin liberar la copia. Como el desktop mapea las vistas de
  todas las superficies cliente, cada launch (fork del desktop) perdia ~4 MiB
  por vista mapeada; tras unos pocos launches se agotaban las paginas fisicas
  y ningun `exec` volvia a funcionar hasta reiniciar (sintoma reportado:
  lanzar una app desde `filesapp` fallaba y ya no arrancaba ninguna app
  grafica). Reproducido y validado headless (QEMU + monitor HMP): antes
  fallaba el tercer launch consecutivo; con el fix cinco launches pasan y el
  fork ya no pierde paginas.
- `desktop_client.path` guardaba el puntero recibido al lanzar: para launches
  pedidos por clientes via `gfx_desktop_launch` (filesapp) apuntaba al buffer
  de stack del request, que muere al volver de
  `service_client_launch_requests`; el titulo de ventana/taskbar y los logs
  leian memoria colgante. Ahora el cliente guarda una copia propia. De paso,
  el hijo del fork loguea el codigo de error real cuando `exec` falla.

## [0.3.1] - 2026-07-02

### Agregado

- **Compositor grafico separado (`/bin/compositord`).** El acceso directo a
  `/dev/gpu0`, import de la superficie de display, presents batched, timeline de
  present y cursor hardware se movieron a un daemon userland propio. `desktop`
  arranca el daemon con pipes y una seccion de framebuffer heredada, y habla un
  protocolo binario versionado definido en
  `subsystems/posix/sdk/v1/include/savanxp/compositor_protocol.h`. El shell de
  escritorio conserva politica de ventanas, menu/taskbar y routing de input, pero
  ya no abre `/dev/gpu0` ni ejecuta ioctls GPU directamente.
- **GPU HAL: backend de display intercambiable.** `namespace display` pasa de ser
  un passthrough fijo a `virtio_gpu` a una indireccion real via `display::Backend`
  (vtable). El registro de `/dev/gpu0` y todo `gpu_ioctl` se movieron a un
  dispatcher agnostico del backend (`kernel/gpu_device.cpp`) que se registra
  siempre y delega la operacion de hardware al backend activo. Nuevo backend
  `kernel/fb_gpu.cpp`: compositor por software directo al framebuffer lineal de
  Limine, sin dispositivo PCI, para correr cuando no hay virtio-gpu (VirtualBox /
  QEMU `-vga std`). `kernel_main` autodetecta: virtio si el probe lo encuentra, si
  no el framebuffer plano. El driver `virtio_gpu` no cambia su logica interna. El
  pendiente historico de F11 sobre framebuffer plano queda resuelto por
  fullscreen compositado por software en el shell.
- **Fullscreen compositado para apps (tecla F11).** Una app marcada como
  fullscreen-capable (Doom, Gfx Demo) pasa a pantalla completa sin chrome: el
  shell escala el buffer 640x400 de la app al framebuffer compartido y presenta el
  resultado via `compositord`, sin cambio de modo ni flip directo de scanout de
  cliente. Esto funciona tanto sobre virtio-gpu como sobre el backend framebuffer
  plano. Las apps fullscreen-capable mantienen un buffer 640x400 tight (la misma
  superficie en ventana y fullscreen). Se conserva el layout v3 con
  `pixels_offset` page-aligned por compatibilidad ABI.
- **Subsistema nativo (Haxe) — puntapie de Fase 0.** Cadena probada end-to-end
  al nivel de compile/link: `Main.hx` -> `reflaxe.CPP` -> C++17 -> clang++
  freestanding -> ELF nativo de SavanXP. Nuevo `subsystems/native/` con un SDK
  semilla (`savanxp_native.h`, envoltura de syscalls `sx_native.c`, entrada
  propia `sx_entry.cpp` que reemplaza el `_main_.cpp` de reflaxe.CPP para evitar
  `<memory>` de libstdc++), un programa Haxe de validacion y un `build.ps1`
  aparte (patron `sdk/doomgeneric`) que clona reflaxe/reflaxe.CPP pineados bajo
  `toolchain/haxe-libs/`, genera el C++ y lo linkea contra el `crt0.S`/`linker.ld`
  del SDK posix. `haxe`/`haxelib` se resuelven via `tools/Toolchain.ps1`.
- **Subsistema nativo — Fase 1: procesos nativos reales.** Un binario nativo se
  marca con `e_ident[EI_OSABI]=0x53` (estampado por el build), el loader lo
  reconoce al cargar la imagen (`elf::LoadResult.os_abi`) y le asigna
  `subsystem::Id::native` segun el ABI del binario, no por herencia del padre
  (aplica a spawn y exec). Sus syscalls entran por `dispatch_native_syscall`, que
  pasa de ENOSYS a estar vivo: delega el baseline en posix (el ABI nativo todavia
  comparte convencion) y queda como punto de divergencia. Verificado en QEMU: el
  ELF Haxe corre con identidad nativa (`process: pid=N marcado nativo` +
  `native: dispatcher activo`) e imprime su salida sin romper el smoke test.
- Tipografias reales horneadas offline a tablas C con `tools/font/genfont.py`
  (via `freetype-py`): **GNU UniFont 8x16** para la consola del kernel y el render
  monospace del terminal, y **Noto Sans** proporcional antialiased para el chrome
  del escritorio y los widgets. El SO sigue sin parsear TrueType en runtime.
- Camino de texto monospace en `sxgfx` (`gfx_blit_text_mono`, `gfx_cell_width/height`)
  y alpha-blending por pixel (`gfx_pixel_blend`) para el texto antialiased de Noto.
- Interrupciones reales por **MSI-X** en `virtio-gpu`: el driver recibe las
  completions por interrupcion (vector del local-APIC) con un patron ISR/DPC
  (la ISR solo marca trabajo y el servicio en background lo drena), en lugar del
  polling puro. El kernel no tiene IOAPIC y q35 en modo APIC no entrega el INTx
  legacy, por lo que MSI-X es el unico camino. Incluye `pci::find_capability`,
  `virtio_pci::enable_msix` y un vector/gate de IDT dedicado (49).
- `poll()` reporta readiness para objetos waitables del kernel (eventos, timers),
  de modo que el compositor puede esperar los eventos de submit de sus clientes
  en el mismo poll set.
- Overlay en pantalla de FPS y latencia de present (promedio/pico de ms
  bloqueado en `gfx_present_region`) en el backend de Doom, estampado directo en
  el framebuffer; y volcado de las stats por etapa del driver (end-to-end,
  transfer/flush/scanout, esperas, timeouts, notificaciones de IRQ) en
  `gputest --soak`.
- Backend de timer `PIT` (8254, IRQ0) como fallback cuando el APIC local no
  soporta x2APIC o falla la calibracion: antes, sin timer activo, el scheduler
  nunca arrancaba en hipervisores que no exponen x2APIC. El vector 32 (IRQ0)
  pasa a compartir el entry point de contexto completo del timer del APIC local
  en vez del dispatcher generico. `savanxp_system_info` expone el backend activo
  (`SAVANXP_TIMER_LOCAL_APIC` / `SAVANXP_TIMER_PIT` / `SAVANXP_TIMER_NONE`) via
  `/dev/sysinfo` y `sysinfo`.

### Cambiado

- El compositor del escritorio compone por **regiones con culling por oclusion**:
  en vez de repintar todas las capas (fondo, clientes, taskbar, menu, cursor) por
  cada rectangulo sucio, arma la lista de capas en z-order y pinta cada una una
  sola vez sobre su region visible (`damage` interseccion `bounds` menos los
  oclusores opacos delante), eliminando el overdraw bajo ventanas opacas. Nuevo
  primitivo de resta de rectangulos `sx_rect_set_subtract_rect` en `sxgfx`; la
  capacidad de `sx_rect_set` sube de 32 a 64.
- La consola del kernel pasa de la fuente bitmap 5x7 autorada a UniFont 8x16, con
  cobertura ASCII + Latin-1 + dibujo de cajas/bloques via una tabla dispersa
  (`include/kernel/console_font_unifont.inc`).
- `gfx_blit_text` ahora rasteriza Noto Sans desde un atlas de cobertura 8-bit; el
  terminal `shellapp` usa el camino monospace UniFont. Se reajustaron las metricas
  de chrome del desktop (`desktop_layout.h`, menu Inicio, accesos directos) para la
  altura de linea mayor.
- UniFont se hornea desde `unifont.hex` (bitmaps nitidos en grilla), no desde el
  outline TTF, que rasterizaba fuera de grilla con artefactos.
- Timer del kernel de 200 Hz a **1000 Hz**, con el quantum del scheduler
  reescalado para mantener ~20 ms de reloj de pared. Senalizar un evento ahora
  cede la CPU al thread despertado en el retorno del syscall (wakeup preemptivo)
  en vez de esperar el proximo tick, recortando la latencia del handshake
  compositor<->cliente.
- El compositor despierta ante el submit de frame de un cliente (sus eventos de
  submit entran al `poll` set) en lugar de agotar el timeout de 16 ms, que queda
  como respaldo.
- El driver `virtio-gpu` pasa a ser interrupt-driven: el spin activo de los
  waiters de present se recorta (50000 a 2000 iteraciones) antes de halt-ear
  (despertado por la IRQ de MSI-X), liberando la CPU; los timeouts de respaldo
  del driver se expresan en milisegundos de reloj de pared (convertidos con la
  frecuencia viva del timer) para sobrevivir al cambio de tick rate.

### Eliminado

- Sistema de fuente 8x8 generado a mano (`tools/font/genfont.ps1`, `font8x8.txt`,
  `gfx_font8x8.inc`), reemplazado por el toolchain de `genfont.py`.

### Corregido

- `release_surface_allocation` (`virtio-gpu`) liberaba el backing de la superficie
  primary sin hacer RESOURCE_UNREF del recurso host, por lo que re-ejecutar
  `configure_primary_surface` en runtime (el primer cambio de modo real via
  `GPU_IOC_SET_MODE`, antes nunca ejercitado) fallaba con `RESOURCE_CREATE_2D` ->
  INVALID_RESOURCE_ID. Ahora destruye el recurso host antes de liberar el backing.
- `decode_bar_size` calculaba el complemento de tamano (`~mask + 1`) en 64 bits
  sobre una mascara con solo los 32 bits bajos, devolviendo tamanos basura
  (`0xffffffff00001000` en vez de `0x1000`) para cualquier BAR de memoria de
  64 bits menor a 4 GiB e impidiendo mapearlos (entre ellos la tabla MSI-X).
  Ahora complementa en 32 bits cuando el tamano entra en 4 GiB.
- `Get-Svfs2BitmapBit`/`Set-Svfs2BitmapBit` (instalador host-side de SVFS2 en
  `tools/UserAppCommon.ps1`) calculaban el indice de byte con `[int]($Bit / 8)`,
  que en PowerShell devuelve un `double` y `[int]` redondea (banker's rounding)
  en vez de truncar: para cualquier bit con `bit % 8 >= 5` (y algunos `= 4`) el
  bit caia en el byte equivocado, desincronizando el inode/block bitmap con el
  indexado floor del kernel (`kernel/svfs.cpp`). El kernel veia esos inodos como
  libres y los reasignaba durante la operacion (p. ej. archivos temporales del
  smoke), zerando el inodo del archivo original y dejando la entrada de
  directorio colgante (sintoma: "inode esperado N pero se leyo 0" tras un ciclo
  de boot). Ahora usan division entera (`-shr 3`).
- Apagado ordenado de QEMU en `Run-AutomationQemu` (smoke/selftest/soak): en vez
  de `Stop-Process -Force`, se pide `quit` por el monitor HMP para que QEMU
  vacie sus backends de bloque y cierre el archivo de disco limpiamente, con
  fallback al kill forzado si el monitor no responde.
- El SO quedaba colgado indefinidamente justo despues de "Iniciando bienvenida"
  en hipervisores sin x2APIC (confirmado en VirtualBox con backend `VBoxVGA`):
  `initialize_local_apic` fallaba en silencio y el scheduler nunca arrancaba, a
  pesar de que el kernel seguia vivo. Resuelto por el fallback a `PIT` agregado
  arriba.
- El framing de paquetes PS/2 del mouse se corrompia en modo streaming:
  `process_mouse_byte` descartaba cualquier byte igual a `0xFA`/`0xFE`
  asumiendo que eran ACK/RESEND de un comando, pero en streaming esos mismos
  valores tambien codifican deltas de movimiento legitimos (`-6` y `-2`
  respectivamente). Al perderse ese byte a mitad de paquete, el framing de 3
  bytes se desincronizaba y el status byte del paquete siguiente terminaba
  interpretado como delta, produciendo saltos erraticos del cursor. El bug era
  direccional: solo se manifestaba moviendo el mouse hacia izquierda/abajo
  (deltas negativos), nunca hacia derecha/arriba (deltas positivos, que nunca
  coinciden con `0xFA`/`0xFE`). Los ACK/RESEND de comandos reales se consumen
  de forma sincronica durante la inicializacion y nunca pasan por este camino.
  Se agrega ademas un clamp defensivo (`+-150` por eje) como red de seguridad
  ante paquetes corruptos futuros: recorta en vez de descartar el paquete
  entero, para no perder tracking en un swipe rapido legitimo.
- `reserve_kernel_mmio_window` reservaba una entrada de PML4 para MMIO del
  kernel sin instalar la tabla PDPT correspondiente, dejando la entrada en `0`
  pese a declararse "reservada"; cualquier `map_kernel_mmio` posterior sobre
  esa ventana fallaba o corrompia memoria. Ahora aloja e instala la PDPT al
  reservar.
- La timeline de present de `fb_gpu` (backend framebuffer plano) devolvia
  siempre `submitted_sequence = 0, retired_sequence = 0`, por lo que
  `wait_present` con un `target_sequence` valido dependia de una coincidencia
  casual en vez de un contador real. Ahora lleva contadores propios de
  secuencia submit/retire por presentacion.
- **Compatibilidad experimental con VirtualBox (backend `VBoxVGA`).** Con los
  fixes de esta version, el sistema arranca de forma estable hasta sesion
  grafica (ya no se cuelga tras el boot) y el mouse PS/2 responde de forma
  correcta en las cuatro direcciones. Pendiente y sin diagnosticar: los
  binarios que viven en `/disk` (`doomgeneric`, `smoke`, `gputest`) no arrancan
  todavia bajo VirtualBox aun adjuntando el volumen SVFS2 (probado convirtiendo
  `build/disk.img` a `.vdi` con `VBoxManage convertfromraw` y adjuntandolo a un
  slot IDE libre); queda para una sesion futura con mas instrumentacion.

## [0.3.0] - 2026-06-18

### Agregado

- Smoke grafico headless automatizado para el compositor: `desktop --selftest`
  arranca el compositor, importa el scanout, lanza un cliente real y valida de
  forma determinista la composicion multi-window y el avance de la timeline de
  presents del GPU (submit/retire), ejercitando ademas las rutas de
  maximizar/restaurar/minimizar/mover ventana. Expuesto como
  `.\build.ps1 desktop-smoke` (token `DESKTOP SMOKE PASS/FAIL`) y enrutado desde
  `init` via el spec `/SMOKE` `desktop-selftest`, cerrando el hueco de validacion
  del camino grafico que antes solo se probaba a mano.
- Apps cliente nuevas del desktop: `filesapp` (navegacion de `/disk` con preview)
  y `aboutapp` (resumen del sistema), cableadas en el menu Inicio.
- Politica y trazabilidad explicita para adopcion de componentes inspirados en
  SerenityOS, con documentacion nueva en `docs/THIRD_PARTY_ADOPTION.md` y
  `docs/THIRD_PARTY_PROVENANCE.md`.
- Capa grafica 2D reutilizable `sxgfx` en el SDK POSIX v1, con `sx_bitmap`,
  `sx_painter`, alpha blending, clipping y `sx_rect_set` para manejar damage
  multiple desde userland.
- Fachada `display` inspirada en `DisplayConnector` sobre el backend
  `virtio-gpu`, con propiedades de conector, import/release de surfaces,
  batching de presents, timeline y eventos waitables exportables a userland.
- Contrato grafico `SAVANXP_GPU_CLIENT_SURFACE_VERSION_3` para apps cliente,
  basado en `section_create/map_view` mas batches de dirty rects y eventos
  `submit/retire/shutdown` en lugar del pipe legacy de presents.
- ABI publica nueva y ampliada en `/dev/gpu0` para seguimiento explicito de
  presents, batching y capacidades del conector, incluyendo timeline,
  waits/events y consultas de propiedades/scanouts sin ownership del display.
- Compositor de escritorio multi-window real, con overlays simultaneos,
  z-order simple, ventana activa, arrastre desde la titlebar y boton de cierre
  en la esquina superior derecha.
- Pipeline de assets bitmap propio para el desktop, con iconos PNG embebidos y
  arte generado dentro del repo para taskbar, menu Inicio, titlebars y franja
  lateral, eliminando la dependencia de assets de SerenityOS.
- Estadisticas ampliadas de `virtio-gpu` con latencia end-to-end por frame,
  incluyendo muestras acumuladas y peor caso observado.

### Cambiado

- El sistema pasa a reportarse como `v0.3.0` en kernel, shell, `uname`,
  `sysinfo`, `aboutapp` y demas componentes que consumen la version compartida.
- El progreso en background de `virtio-gpu` deja de depender del subsistema de
  input y pasa a bombearse desde un servicio de dispositivos del kernel
  invocado en timer y waits bloqueantes.
- El `desktop` deja atras el modelo de un solo cliente fullscreen y pasa a un
  compositor por surfaces con invalidacion por multiples rectangulos,
  composicion por clipping y presents batched hacia el scanout principal.
- El loop principal del `desktop` se desacopla en layout/render/menu/session,
  manteniendo el binario unico pero separando mejor la responsabilidad del
  compositor, el shell de fondo y las ventanas overlay.
- `shellapp`, `doomgeneric` y las demas apps cliente migran al canal async de
  surfaces version 3; el terminal fuerza redraw completo cuando el scroll mueve
  el historial para evitar artefactos visuales en la ventana.
- La sincronizacion compositor-GPU pasa a usar timeline explicita y retiro del
  frame anterior antes de reciclar el backbuffer visible, reduciendo tearing
  logico y mejorando el pacing del desktop.
- El menu Inicio y la taskbar reciben varias pasadas de pulido visual y de
  comportamiento: layout mas limpio, sidebar bitmap, textos que caben mejor,
  hover estable y mejor feedback del cliente activo.
- Las ventanas overlay ya pueden moverse dentro del area util del desktop y
  cerrarse directamente desde su titlebar con una cruz clasica estilo shell.
- El arte embebido del desktop pasa a generarse desde assets propios del repo,
  sustituyendo las referencias temporales usadas durante el prototipado
  inspirado en SerenityOS.
- `gputest --smoke` ahora valida tambien la timeline de presents y el pacing
  explicito del driver, no solo stats/stages internos.
- El flujo de `build/disk.img` persistente se vuelve a validar despues de cada
  tanda grande de cambios con `doomgeneric` como prueba real de no regresion.
- `virtio-gpu` reorganiza su estado interno alrededor de un `Adapter` con
  subestados separados para transporte, display, cursor, presents y runtime,
  dejando mejor preparada la base para locking fino y recovery mas predecible.
- El trabajo en background de `virtio-gpu` queda separado en fases explicitas
  de drain de colas, avance del pipeline y procesamiento de eventos de config,
  con serializacion atomica corta para submit/drain de virtqueues.
- La timeline de presents pasa a reconocer `present_cookie` como correlacion
  real tambien cuando hay coalescing, retiro por rangos y batching de damage.
- El camino parcial de presentacion ya puede actualizar el front buffer activo
  sin clone completo cuando el recurso esta idle, y los batches importados
  conservan rects reales para `TRANSFER_TO_HOST_2D` y `RESOURCE_FLUSH` antes
  de caer a bounding rect solo si se agota la capacidad interna.
- El `desktop` empieza a usar el evento waitable de present exportado por
  `/dev/gpu0` como hint de readiness para reducir polling innecesario de la
  timeline, manteniendo `WAIT_PRESENT` como sincronizacion fuerte.
- Los eventos de display/scanout de `virtio-gpu` endurecen su handling:
  un refresh fallido ya dispara degradado y recovery deliberado en lugar de
  quedar como fallo silencioso del camino de hotplug/config.
- `gputest --smoke` sube la cobertura del driver validando handles waitables de
  present y scanout, junto con un soak liviano de presents parciales y refreshes
  repetidos para detectar regresiones de pacing y recovery mas temprano.
- Los timeouts de reserva de superficie y de slot pending en `virtio-gpu`
  pasan a tratarse como sintomas de atasco real del pipeline, entrando en modo
  degradado e intentando recovery igual que los otros waits criticos.
- El recovery de `virtio-gpu` evita reentradas simultaneas y `gputest --smoke`
  endurece la cobertura de eventos comprobando tambien que los handles vuelvan
  a quedar sin senal despues de `event_reset`, con un soak algo mas agresivo.
- `gputest` agrega un modo dedicado `--soak` para ejercer durante mas tiempo el
  backend `/dev/gpu0` con mezcla determinista de full presents, partial presents,
  waits por evento y refreshes de scanout, sin volver mas lenta la smoke normal.
- `build.ps1` agrega el comando `gpu-soak`, reutilizando `/SMOKE` como selector
  de runner para que `init` pueda lanzar `gputest --soak` en QEMU y reportar
  tokens `SOAK PASS/FAIL` aptos para validacion automatizada.
- El recovery de `virtio-gpu` deja de ser tan global en dominios no criticos:
  un fallo al rearmar el cursor ahora degrada a software cursor y las imported
  surfaces no criticas pueden descartarse localmente durante recovery en lugar
  de voltear toda la rearmada del dispositivo.
- Los refreshes de scanout de `virtio-gpu` pasan a ser transaccionales sobre el
  cache de conectores: si el host entrega un evento incompleto o falla el rearm
  del primary, el driver restaura el estado anterior y solo escala a recovery
  global cuando ni siquiera puede mantener el scanout primario.
- `gputest --soak` ahora acepta cantidad de iteraciones y cubre tambien imported
  surfaces mediante `GPU_IOC_IMPORT_SECTION` y `GPU_IOC_PRESENT_SURFACE_BATCH`,
  con `build.ps1 gpu-soak -GpuSoakIterations N` para repetir tandas mas largas
  sin tocar la smoke normal.
- El compositor `desktop` corrige el pacing de sus presents importados usando la
  timeline real para generar `present_cookie`, evitando retirar frames antes de
  que el GPU termine y reduciendo el tearing visible en apps pesadas.
- El runtime `gfx` del SDK deja de exponer como backbuffer directo el mismo
  buffer compartido que lee el compositor: cada cliente dibuja en un backbuffer
  privado y el runtime copia al surface compartido solo cuando el frame anterior
  ya fue compuesto, eliminando backlog de frames sin esperar el retiro final del
  GPU.
- El protocolo de surfaces cliente agrega `composed_sequence` para separar
  composicion del desktop y retiro del GPU; el compositor senala progreso apenas
  copia el frame al backbuffer visible y conserva `retired_sequence` para el
  retiro real del present.
- El `desktop` limita el drenaje de eventos de mouse por frame para que arrastrar
  ventanas no acumule un backlog grande antes de volver a componer.
- `poll()` deja de tratar todos los devices como siempre legibles: `/dev/input0`
  y `/dev/mouse0` exponen readiness real de cola, reduciendo el busy-loop del
  compositor cuando no hay eventos pendientes.
- El manejo de mouse del `desktop` pasa a leer bloques por frame y coalescer
  movimientos consecutivos con el mismo estado de botones, siguiendo el patron
  de WindowServer de SerenityOS para no componer un frame por cada paquete crudo.
- `sx_rect_set` corrige el merge de rectangulos adyacentes para no unir areas
  separadas que solo coinciden en una coordenada de borde, evitando dirty rects
  artificialmente enormes al mover ventanas.
- `doomgeneric` y el compositor imprimen ahora el error concreto de present o
  batch invalido cuando una surface cliente falla, facilitando diagnostico de
  regresiones visuales.

## [0.2.2] - 2026-04-01

### Agregado

- Arbol nuevo `subsystems/` con `subsystems/posix` como primer subsistema
  explicito del SO, separando `kernel`, `userland` y `sdk` bajo un mismo
  ownership.

### Cambiado

- La entrada y el dispatcher POSIX de syscalls pasan a vivir bajo
  `subsystems/posix/kernel`, mientras `kernel/` conserva el scheduler,
  procesos base, VM, VFS, drivers y demas mecanismos genericos.
- El SDK canonico v1 pasa a `subsystems/posix/sdk/v1`; el build principal,
  `tools/build-user.ps1` y la extension de VS Code consumen ahora esa ruta
  como referencia publica del subsistema POSIX.
- El userland interno se mueve a `subsystems/posix/userland` y compila contra
  el runtime canonico compartido del SDK, eliminando duplicados internos y
  dejando `sdk/` top-level como raiz de ejemplos y ports.
- La definicion publica del ABI visible queda unificada en
  `subsystems/posix/sdk/v1/include/savanxp/syscall.h`, sin copia paralela en
  `include/shared`.
- El sistema pasa a reportarse como `v0.2.2` en kernel, shell, `uname`,
  `sysinfo` y componentes que consumen la version compartida.
- La migracion vuelve a validar el flujo de imagen persistente:
  `.\sdk\doomgeneric\build.ps1` y `.\build.ps1 build` siguen conservando
  `doomgeneric` y `doom1.wad` en `build/disk.img` sin recreacion normal de la
  imagen.

## [0.2.1] - 2026-03-30

### Agregado

- ABI publica nueva en `/dev/gpu0` para diagnostico y control 2D extendido:
  `GPU_IOC_GET_STATS`, `GPU_IOC_GET_SCANOUTS`,
  `GPU_IOC_REFRESH_SCANOUTS`, `GPU_IOC_SET_CURSOR` y
  `GPU_IOC_MOVE_CURSOR`.
- Estadisticas ampliadas de `virtio-gpu` para presents, stages, waits,
  timeouts, completions, IRQs, recovery y operaciones de cursor.
- Enumeracion de scanouts y refresh basico de display info/hotplug en el
  backend `virtio-gpu`, manteniendo `desktop` single-display por defecto.
- Soporte inicial de cursor plane por hardware en `virtio-gpu`, con fallback
  transparente al cursor software del `desktop` cuando el backend no lo
  expone o falla.
- Coverage automatizado adicional en `gputest --smoke` para validar progreso
  del driver via `GPU_IOC_GET_STATS` y enumeracion de scanouts.

### Cambiado

- El sistema pasa a reportarse como `v0.2.1` en kernel, shell, `uname`,
  `sysinfo` y componentes que consumen la version compartida.
- El modelo grafico normal queda definitivamente desktop-first: la taskbar
  permanece visible, las apps cliente renderizan sobre un work area estable y
  el `desktop` pasa a ser el dueño normal del scanout.
- `shellapp`, `gfxdemo`, `keytest`, `mousetest` y `doomgeneric` quedan
  alineados al camino cliente del compositor en vez del fullscreen directo
  legacy sobre `/dev/gpu0`.
- La taskbar y el menu Inicio reciben una pasada de pulido visual y de
  comportamiento para encajar mejor con el nuevo contrato desktop-first.
- El backend `virtio-gpu` deja de depender de reentradas oportunistas del
  caller para progresar: el scheduler interno ahora coalescea presents por
  recurso, reduce `SET_SCANOUT` redundantes y avanza trabajo en segundo plano
  con apoyo de IRQ cuando la linea PCI esta disponible.
- `virtio-gpu` agrega recovery deliberado y modo degradado predecible frente
  a timeouts del device, intentando restaurar el scanout primario y la
  consola sin exigir reinicio inmediato del SO.
- El build principal y la tooling asociada dejan mas explicito que
  `build/disk.img` es persistente por defecto: se valida consistencia de
  `SVFS2`, se evita recrear la imagen salvo corrupcion real y se conserva
  `doomgeneric` junto con `doom1.wad` como regresion practica de
  persistencia.
- Los perfiles de QEMU usados por `run`, `smoke` y las utilidades graficas se
  alinean mejor con el hardware virtual real del stack actual
  (`virtio-gpu` + `virtio-tablet` + `desktop`).
- `doomgeneric` pasa a vivir definitivamente como cliente del compositor y
  queda orientado a validacion manual dentro de la sesion grafica normal,
  en vez de depender de una smoke host-side propia.

## [0.2.0] - 2026-03-22

### Agregado

- Sesion `desktop-first` con compositor `desktop`, shell fullscreen
  `shellapp`, surfaces de cliente compartidas por `SectionObject` y
  lanzamiento de apps graficas via `fd 3..6`.
- ABI publica extendida en `/dev/gpu0` con `GPU_IOC_SET_MODE`,
  `GPU_IOC_IMPORT_SECTION`, `GPU_IOC_RELEASE_SURFACE`,
  `GPU_IOC_PRESENT_SURFACE_REGION` y `GPU_IOC_WAIT_IDLE`.
- ABI publica nueva para audio con `SAVANXP_IOCTL_GROUP_AUDIO`,
  `AUDIO_IOC_GET_INFO` y `struct savanxp_audio_info`.
- Driver `virtio-sound` playback-only sobre `virtio_pci`, exponiendo
  `/dev/audio0` con formato fijo `S16LE stereo 48 kHz`.
- Utilidad nueva `audiotest` y coverage automatizado en `.\build.ps1 smoke`
  para validar `/dev/audio0`.
- Object manager minimo con handles kernel genéricos para I/O, eventos,
  timers y sections.
- Syscalls nuevas `EVENT_*`, `WAIT_ONE`, `WAIT_MANY`, `TIMER_*`,
  `SECTION_CREATE`, `MAP_VIEW` y `UNMAP_VIEW`, con wrappers actualizados en
  userland y SDK v1.
- Soporte inicial de `Section/View` anónimo en el kernel, incluyendo memoria
  compartida entre procesos, herencia `shared` vs `private` al hacer `fork()`
  y tests nuevos `eventtest`, `timertest`, `sectiontest` y `mmaptest`.
- Capa POSIX nueva para `mmap` / `munmap` anónimo en `subsystems/posix/sdk/v1`, más header
  estándar `sys/mman.h`.

### Cambiado

- El sistema pasa a reportarse como `v0.2.0` en kernel, shell, `uname`,
  `sysinfo` y componentes que consumen la version compartida.
- El arranque normal pasa a supervisar `desktop` desde `init`; `/SMOKE` sigue
  evitando el desktop y mantiene la smoke automatizada headless.
- `gfx_open` y el runtime grafico pasan a ser compositor-first, con fallback
  directo sobre `/dev/gpu0`; el nodo legacy `/dev/fb0` deja de exponerse en
  el sistema actual.
- El build principal ahora instala el multicall BusyBox portado en `/bin` y
  `/disk/bin` para `ls`, `cat`, `echo`, `mkdir`, `rm`, `mv`, `cp` y
  `ps`.
- `virtio-gpu` pasa a presentar sobre un set interno de tres superficies y el
  camino legacy `FB_IOC_*` sale de la ABI vigente.
- Los perfiles `run` y `smoke` de QEMU agregan `virtio-sound-pci` con un
  `audiodev` separado del camino de `pcspeaker`.
- `sleep_ms()` ahora corre sobre timers waitables del kernel en vez de un
  camino especial separado, y `fork()` preserva views anónimas compartidas o
  privadas segun el tipo de mapping.
- El menu Inicio deja de ofrecer `Exit Desktop`, y `shellapp` puede cerrarse
  con `exit` para volver al desktop y reabrirse despues desde `Menu -> Shell`.
- El compositor desktop reduce algo de trabajo redundante en el camino de
  presentacion y corrige mejor el enrutado/polling de input para clientes
  fullscreen.

## [0.1.4] - 2026-03-19

### Agregado

- Syscalls y wrappers POSIX nuevos para `fork`, `kill`, `raise`, `poll`,
  `select` y `fcntl(F_GETFL/F_SETFL)` con soporte de `O_NONBLOCK`.
- Runner automatizado `.\build.ps1 smoke`, que recompila, instala en
  `/disk/bin`, arranca QEMU headless y valida `fork`, señales basicas, polling
  y persistencia real sobre `SVFS2`.
- Userland multicall `busybox` para empezar el reemplazo de utilidades
  comodin, incluyendo `echo`, `cat`, `ls`, `mkdir`, `rm`, `mv`, `cp`, `true`,
  `false` y `sleep`.

### Cambiado

- El sistema pasa a reportarse como `v0.1.4` en kernel, shell, `uname`,
  `sysinfo` y componentes que consumen la version compartida.
- El timer base del sistema pasa a calibrarse con objetivo de `200 Hz` en vez
  de `100 Hz`, mejorando un poco la respuesta percibida del mouse y el
  redondeo practico de `sleep_ms()` para loops graficos e input.
- Los techos internos suben para procesos, descriptores, pipes, sockets, VFS
  y `SVFS2`, dejando mas margen para ports y userland real.
- `SVFS2` ya puede montar `/disk` en modo degradado de solo lectura si la
  recuperacion no deja al volumen seguro para `RW`, evitando que quede
  directamente offline frente a fallos recuperables.
- El build principal instala tambien los binarios internos en `/disk/bin`, de
  modo que la shell y la smoke automatizada ejercitan la misma copia
  persistente del userland.

## [0.1.3] - 2026-03-17

### Agregado

- Base compartida `virtio_pci` para drivers `virtio` modernos sobre PCI/MMIO,
  reutilizada por `virtio-input` y preparada para colas sincronas por polling.
- Driver nuevo `virtio-gpu` 2D para QEMU, con soporte MVP de
  `GET_DISPLAY_INFO`, `RESOURCE_CREATE_2D`, `RESOURCE_ATTACH_BACKING`,
  `SET_SCANOUT`, `TRANSFER_TO_HOST_2D` y `RESOURCE_FLUSH`.
- Nodo nuevo `/dev/gpu0` con ABI publica `GPU_IOC_GET_INFO`,
  `GPU_IOC_ACQUIRE`, `GPU_IOC_RELEASE`, `GPU_IOC_PRESENT` y
  `GPU_IOC_PRESENT_REGION`.
- Utilidad nueva `gputest` para validar el camino directo de presentacion
  sobre `/dev/gpu0`.
- Calibracion del timer `local APIC/x2APIC` contra `RTC/CMOS` durante el boot
  para que `uptime_ms` y `sleep_ms` queden mejor alineados con tiempo real en
  QEMU.

### Cambiado

- `/dev/fb0` conserva compatibilidad con las apps fullscreen existentes, pero
  ahora puede presentar sobre `virtio-gpu` cuando el backend queda disponible.
- El perfil de QEMU en `build.ps1 run` ahora agrega `virtio-vga` con
  `xres=1280,yres=800`, y `limine.conf` pide `1280x800x32` para que el
  framebuffer de boot y el backend grafico queden alineados durante el
  handoff.
- La consola y la UI fullscreen pueden seguir visibles sobre el recurso
  primario de `virtio-gpu`, incluyendo el retorno desde sesiones graficas
  exclusivas y el redraw limpio de toda la shell sin dejar residuos en los
  margenes.
- `virtio-gpu` ahora intenta conservar el modo grande del framebuffer de boot
  antes de caer al scanout nativo reportado por el dispositivo, evitando que
  el sistema vuelva a `640x480` al finalizar el arranque cuando el modo mayor
  es aceptado.
- `virtio-input` paso a usar la geometria efectiva del framebuffer activo para
  normalizar el tablet absoluto, corrigiendo la desincronizacion del mouse con
  el host despues del cambio a `virtio-gpu`.
- El heap del kernel dejo de ser lineal-only y ahora recicla bloques
  liberados, hace `split/coalesce` y puede devolver arenas completas al
  allocador fisico cuando quedan vacias.
- El runtime POSIX de `subsystems/posix/sdk/v1` reemplazo su allocator tipo arena/bump por un
  heap fijo reciclable, de modo que `malloc`, `free`, `calloc` y `realloc`
  ya reutilizan memoria en apps externas.
- `sdk/doomgeneric` ya no corre acelerado por un reloj base incorrecto; el
  tiempo del juego vuelve a apoyarse sobre un backend de tiempo mas cercano al
  real, dejando el tuning de rendimiento restante del lado del port.

### Limites conocidos

- Este MVP de `virtio-gpu` mejora de forma visible la GUI fullscreen en QEMU,
  pero la subida de pixeles sigue siendo sincronica, con copia por CPU y sin
  `mmap`, page flipping ni doble buffer real.
- Portar apps a `/dev/gpu0` reduce capas y deja mejor encaminada la evolucion,
  pero la mejora grande en suavidad queda para una etapa posterior con buffers
  compartidos y presentacion menos bloqueante.
- El rendimiento final de ports externos grandes como `sdk/doomgeneric`
  todavia depende mucho del costo de escalado y del tamano del frame una vez
  que el sistema ya corre a resoluciones mas altas.

## [0.1.2] - 2026-03-14

### Agregado

- Soporte basico de mouse `PS/2` sobre el puerto auxiliar del controlador
  `i8042`, con IRQ12, paquetes estandar de 3 bytes y degradacion segura a
  teclado-only si el mouse no inicializa.
- Nodo nuevo `/dev/mouse0` con eventos dedicados de mouse para apps graficas,
  sin romper la semantica previa de `/dev/input0`.
- ABI compartida extendida con `struct savanxp_mouse_event`, flags publicos de
  botones y helpers nuevos `mouse_open` / `mouse_poll_event` en la libc/runtime.
- Nuevo shell grafico fullscreen `desktop`, inspirado en el lenguaje visual de
  Windows 2000, con taskbar, boton Inicio, reloj y cursor.
- Nueva utilidad `mousetest` para validar `/dev/mouse0`, movimiento relativo y
  botones desde userland.

### Cambiado

- La capa fullscreen del kernel ahora registra `/dev/mouse0` junto con
  `/dev/fb0` y `/dev/input0`, y limpia colas de teclado/mouse al adquirir o
  liberar la sesion grafica exclusiva.
- Bajo QEMU, el escritorio y `mousetest` ahora priorizan un backend absoluto
  `virtio-tablet-pci` cuando esta disponible, mientras `/dev/mouse0` conserva
  la ABI de deltas para no romper apps ya compiladas.
- El kernel ahora reserva una ventana MMIO propia para drivers PCI modernos y
  la usa para mapear BARs de memoria de forma segura durante el boot.
- En entornos QEMU con `virtio-tablet-pci`, el stack `PS/2` deja de inicializar
  el mouse auxiliar y queda teclado-only; el mouse `PS/2` se conserva como
  fallback cuando `virtio-input` no esta disponible.
- Se agrego lectura de `RTC/CMOS` en el kernel y un helper publico aditivo para
  consultar hora real desde userland; el reloj del escritorio ya no depende
  solo de `uptime`.
- La shell builtin ahora publica `desktop` y `mousetest` dentro del help
  interactivo.
- La documentacion principal y la referencia del SDK v1 reflejan el nuevo
  input de mouse, el escritorio inicial y el salto a `v0.1.2`.

### Limites conocidos

- `v0.1.2` expone solo movimiento relativo y botones basicos en la ABI
  publica; internamente puede usar puntero absoluto bajo QEMU, pero no hay
  rueda, ventanas reales, compositor ni input raw para juegos.
- `gfx_poll_event` sigue siendo teclado-only en esta etapa; el mouse entra por
  `/dev/mouse0`.

## [0.1.1] - 2026-03-10

### Agregado

- `SVFS2` como nueva version del filesystem persistente de `/disk`, con
  `superblock` primario/secundario, journal fijo de metadatos, bitmap de
  bloques, bitmap de inodos y tabla de inodos con extents.
- Syscall nueva `sync` y comando userland `sync` para forzar checkpoint
  explicito del estado persistente.
- Red minima sobre `rtl8139` + `QEMU user-net`, con `ARP`, `IPv4`, `ICMP`
  echo request/reply, sockets UDP IPv4 basicos y cliente TCP minimo.
- ABI publica extendida para dispositivos y `ioctl`, con nodos `/dev/fb0`,
  `/dev/input0`, `/dev/net0` y `/dev/pcspk`.
- GUI fullscreen inicial con primitivas `gfx_*`, demo interna `gfxdemo` y
  ejemplo externo `sdk/gfxhello`.
- Sonido minimo por `PC speaker` con comando `beep`.
- Primer port externo grande en `sdk/doomgeneric`, usado como hito practico
  de apps externas graficas sobre el ABI del sistema.
- Capa POSIX/libc inicial para SDK v1 con headers estandar publicos:
  `unistd.h`, `fcntl.h`, `stdio.h`, `stdlib.h`, `string.h`, `dirent.h`,
  `sys/stat.h`, `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`, `time.h`
  y asociados.
- Runtime nuevo `subsystems/posix/sdk/v1/runtime/posix.c` para apps externas, con `stdio`
  basico, `DIR*`, heap simple tipo arena, conversiones, helpers de string,
  tiempo y sockets cliente.
- Syscalls/base ABI nuevas para `getpid`, `stat`, `fstat`, `chdir`
  y `getcwd`.
- Smoke test externo `sdk/posixsmoke`, compilado solo contra headers
  estandar.
- Utilidad `keytest` para inspeccionar eventos de teclado sobre `/dev/input0`
  en fullscreen y validar `key down/up`, `keycode` y `ascii`.
- `FB_IOC_PRESENT_REGION` como extension de la ABI grafica para presentar solo
  una region del framebuffer desde userland.

### Cambiado

- La capa VFS ahora centraliza la normalizacion de paths y amplio la capacidad
  de rutas internas a `256` bytes, de modo que `process`, `cwd` y operaciones
  de filesystem comparten una sola resolucion canonica.
- El tooling host sobre `build/disk.img` (`build.ps1`, `tools/build-user.ps1`
  y `tools/UserAppCommon.ps1`) dejo de escribir `SVFS1` y paso a crear e
  instalar directamente sobre imagenes `SVFS2`.
- Las rutas persistentes de `/disk` dejaron de depender de nombres-path como
  identidad on-disk y pasaron a montarse desde inodos estables cacheados en
  memoria del kernel.
- El kernel ahora resuelve paths relativos contra un `cwd` por proceso, de
  modo que `open`, `exec`, `spawn` y operaciones de filesystem comparten
  directorio actual.
- El stack de teclado `PS/2` se reforzo con init del controlador mas robusta,
  decodificacion desacoplada de `TTY`/`UI`, mejor soporte de `AltGr`, locks y
  teclas especiales.
- `sdk/doomgeneric` dejo de depender de su set privado de headers estandar y
  paso a consumir la capa publica del SDK, reduciendo `savanxp_compat.c` a
  glue especifico del port.
- Las primitivas `gfx_*` del runtime compartido se optimizaron para trabajar
  con spans/rectangulos contiguos y reducir el costo de dibujo por frame.
- `gfxdemo`, `sdk/gfxhello` y `keytest` dejaron de refrescar la pantalla
  completa en cada iteracion y ahora usan regiones sucias o redraw bajo
  demanda para mejorar fluidez en fullscreen.
- El backend de `sdk/doomgeneric` reemplazo el escalado basado en divisiones
  por pixel por una expansion por filas cacheadas, bajando el costo de CPU por
  frame durante el render fullscreen.
- La documentacion principal y la referencia del SDK se actualizaron para
  reflejar la superficie POSIX/libc disponible y sus limites practicos.

### Limites conocidos

- La validacion reciente del salto a `SVFS2` cubre compilacion completa y
  verificacion host-side del flujo `build-user`, pero todavia no incluye
  smoke tests de reboot/replay dentro de QEMU.
- Si la recuperacion del journal o del metadata base falla al montar, el
  volumen queda offline; aun no existe un modo degradado de solo lectura.
- `free()` todavia no recicla memoria; el allocator userland sigue siendo de
  tipo arena/bump.
- `DIR->d_type` se completa por `stat()` best-effort en userland.
- `setsockopt`/`getsockopt` cubren solo flags y timeouts basicos del cliente.
- Segun el host y la captura de teclado de QEMU, `ImpPnt` puede no entrar al
  guest como tecla dedicada y requerir `Alt+ImpPnt` para pruebas manuales.
- La validacion reciente de `v0.1.1` es host-side; el smoke POSIX nuevo aun no
  fue corrido dentro de QEMU en esta tanda.

## [0.1.0] - 2026-03-08

Primera version publicada del experimento.

### Agregado

- Bootstrap del kernel sobre `x86_64 + UEFI + Limine`, con recepcion de
  `bootloader info`, `framebuffer`, `memory map`, `HHDM` e `initramfs`.
- Consola de texto sobre framebuffer con scroll, cursor y salida serie
  temprana por `COM1` / `debugcon`.
- GDT/IDT con segmentos de usuario, `TSS`, excepciones basicas y puerta de
  syscall por `int 0x80`.
- Allocador fisico temprano, heap del kernel y VMM minimo para espacios de
  usuario.
- Driver de teclado `PS/2`, `TTY` canonica y shell interactiva inicial.
- `VFS` minima montando `initramfs` `cpio newc`, con archivos dinamicos en
  memoria y volumen persistente `SVFS` montado en `/disk`.
- Loader `ELF64` estatico para procesos simples en `ring 3`.
- Scheduler round-robin preemptivo con bloqueo por `wait`, `read` y `sleep`.
- Shell con `pipes`, redireccion (`|`, `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`),
  parser de comillas simples/dobles y builtins `exec`, `which` y `mkdir`.
- Handles refcounted con `dup`, `dup2`, `waitpid(-1)` y procesos zombie/reap.
- Reclaim de paginas en `exit`/`exec`, destruccion de `VmSpace` y liberacion
  de stacks de kernel al reapear procesos.
- `SVFS` con subdirectorios persistentes simples bajo `/disk`, `mkdir`,
  `rmdir` de directorios vacios, `truncate` explicito y `rename`
  persistente.
- SDK v1 minima en `C`, con `crt0`, `libc`, linker script, headers
  `savanxp/*`, tooling host para instalar apps externas en `build/disk.img`
  y ejemplos base (`sdk/hello`, `sdk/errdemo`, `sdk/fsdemo`, `sdk/pathops`,
  `sdk/procpeek`, `sdk/spawnwait`, `sdk/statusdemo`, `sdk/multifile`,
  `sdk/template`).
- Userland inicial con `init`, `sh`, `echo`, `uname`, `ls`, `cat`, `sleep`,
  `ticker`, `demo`, `true`, `false`, `ps`, `fdtest`, `waittest`,
  `pipestress`, `spawnloop`, `badptr`, `rm`, `rmdir`, `truncate`,
  `seektest`, `truncatetest` y `errtest`.

### Cambiado

- Se consolidaron procesos, pipes y persistencia para que `/disk` quede
  operativo como flujo principal de trabajo entre reinicios.
- La superficie de syscalls y la `libc` minima se ampliaron con operaciones de
  filesystem y proceso necesarias para shell, pipes y apps externas.
- La documentacion del repo y del SDK v1 se congelo para dejar una base
  publica util a partir de la primera version numerada.
