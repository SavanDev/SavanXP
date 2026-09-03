# Changelog

Registro de cambios visibles por version para SavanXP.

Notas de corte:

- `v0.1.0` se reconstruyo de forma retroactiva desde el historial hasta `d822857`.
- `v0.1.1` cubre los cambios posteriores a `v0.1.0`, incluyendo el trabajo actual
  ya integrado en el arbol pero todavia no etiquetado en git.

## [Unreleased]

### Agregado

- **Selector de layout de teclado ES/EN, en la barra de tareas.** El unico mapa
  de teclas que existia era el ES horneado en `kernel/ps2.cpp`. Ahora hay dos
  -- se suma el US QWERTY estandar -- y se elige en caliente por un ioctl sobre
  `/dev/input0` (`INPUT_IOC_SET_LAYOUT`/`GET_LAYOUT`, el mismo patron que ya
  usan la GPU, el portapapeles y el speaker). La eleccion se guarda en
  `/disk/keyboard.cfg` y se aplica en el boot, antes de windowd, asi que la
  sesion arranca con el que se dejo puesto. La UI es un indicador ES/EN en la
  barra que abre un popup anclado arriba de la franja (`kbdlayoutpopup.c`): es
  un tipo de cliente nuevo de windowd, no una extension del protocolo WM
  generico. Verificado con capturas reales -- `tools/shoot.ps1 -Scenario
  kbdlayout`, escena nueva -- y con `kbd-smoke`, `windowd-smoke` y
  `taskbar-smoke` en verde.

- **`build.ps1 kbd-smoke`: el driver de teclado se prueba solo.** Cierra el
  camino real PS/2 -> IRQ -> `kernel/ps2.cpp` -> `/dev/input0`, que ni
  `windowd --selftest` ni guihost cubren porque inyectan el evento ya formado
  -- es la misma clase de agujero por el que se colo el bug de Ctrl+C.
  `kbdtest` adquiere la sesion grafica, como gputest, lee `/dev/input0` de
  verdad y compara contra un guion de checkpoints (letra simple, Shift, Ctrl,
  extendida, Enter) mientras `tools/kbd_smoke.py` mueve el teclado emulado por
  QMP, apenas el serial anuncia `KBD SMOKE READY`. `Run-AutomationQemu` suma
  `-ReadyToken`/`-OnReady` para ese enganche, sin tocar los harnesses que ya
  existian.

- **El estampado SXE deja de ser opt-in: todo lo que se compila para el sistema
  ya sale en ese formato.** Cierra la brecha entre "el formato existe" y "cada
  programa que se compile ya esta en el formato": la cobertura medida sobre la
  imagen completa es de 68/68 binarios instalados, in-tree y externos (Doom,
  busybox) por igual. Es el mismo rol que cumple el bloque VERSIONINFO que el
  linker de Windows agrega aunque el programador nunca haya escrito un `.rc`:
  identidad minima que sale del build y no del programador. Cada programa
  recibe un `.sxmeta` tenga o no `.sxres`, con nombre, version del sistema,
  subsistema y el commit corto como `BUILD_ID`; nunca se inventan icono,
  accent, descripcion ni mimes -- eso sigue siendo enriquecimiento del
  `.sxres`. El nombre automatico es a proposito EL MISMO texto que progman y
  windowd ya mostraban como fallback del basename, asi que para un binario sin
  manifiesto no cambia una sola ventana: solo deja de inferirse en cada
  arranque y pasa a estar declarado una vez, en el build. Ademas el generador
  ya no decide para quien generar mirando que `.sxres` hay en el disco -- eso
  invertia el control --, sino que recibe la lista de programas de quien sabe
  la verdad, y avisa si un `.sxres` no le corresponde a ninguno: el error mas
  probable ahi es un `.c` renombrado sin renombrar su manifiesto.

- **`icon_file=` en el manifiesto: el icono viaja en el binario y no en
  `assets/`.** El generador solo sabia resolver `icon=` por nombre contra
  `assets/desktop/icons/`, asi que para estampar un icono propio primero habia
  que meter el arte en el arbol del sistema -- lo contrario de lo que quiere el
  formato. Se nota sobre todo con un port de terceros: `assets/` se versiona y
  se hornea en la imagen, asi que pasar por ahi significa distribuir arte ajeno
  con el sistema para poder mostrarlo en el launcher. `icon_file=` toma un PNG
  relativo al `.sxres` y deriva los dos tamanos que exige el runtime: si el
  original es multiplo entero del destino usa NEAREST y el pixel art sale
  intacto, y si no lo es, un remuestreo suave, porque NEAREST con una razon no
  entera se come filas de a una y queda peor. Las dos claves son excluyentes y
  el generador falla si estan las dos: el icono sale del catalogo del sistema o
  de un PNG propio, no de los dos.

- **Punto flotante en userland, con `-Sse` y una libm propia.** Hasta ahora el
  codigo con `float`/`double` compilaba pero NO linkeaba, y el sintoma caia
  lejos de la causa: con `-mno-sse -mgeneral-regs-only` clang no se niega, pasa
  los `double` en registros de proposito general -- una convencion propia, no
  la de System V -- y resuelve cada operacion llamando a los helpers de
  soft-float de compiler-rt (`__adddf3`, `__mulsf3`), que este sistema no
  tiene; el resultado era un simbolo indefinido en el link. Del lado del SO no
  faltaba nada: el kernel ya guardaba y restauraba el area FPU/SSE por proceso
  en cada cambio de contexto (`fxsave64`/`fxrstor64`) y `crt0.S` ya dejaba
  `rsp` alineado a 16. El switch es opt-in porque una app que no usa floats no
  gana nada y prefiere que el compilador no le meta SSE en un memcpy
  vectorizado -- el camino in-tree de `build.ps1` sigue en `-mno-sse` --, y
  `math.h` declara la biblioteca bajo `#if defined(__SSE2__)` para que sin el
  switch el error salga al COMPILAR nombrando la funcion, en vez de un
  `__adddf3` sin resolver. Dos decisiones del contenido de la libm:
  `floor`/`ceil`/`trunc` se hacen con bits y no con `roundsd`, que es SSE4.1 y
  el modelo de CPU con el que arranca QEMU por defecto no expone; y `fmod`,
  `atan2`, `exp`, `log` y `pow` se apoyan en el x87, que sigue vivo con SSE
  prendido y da 64 bits de mantisa, fijando la precision del control word a
  extendida y reponiendola al salir -- `fldl2e` y companiaa cargan la constante
  ya redondeada a la precision vigente, asi que en un entorno que la dejo en 53
  bits `exp` arrancaba con un `log2(e)` degradado y el error se amplificaba con
  |x| (435 ulp en `exp(-678)`; con la precision fijada, 1). Validado de dos
  formas: contra la libm del host, con todo el conjunto en 1 ulp y
  `floor`/`ceil`/`trunc`/`round`/`fabs`/`sqrt`/`fmod` exactos bit a bit, y
  adentro del SO con `build.ps1 float-smoke`, que ademas cubre lo unico que no
  se puede comprobar compilando: que el estado FPU/SSE sobreviva a un cambio de
  contexto.

- **Control de pestanias en sxgui (`sxgui_tabs`).** La pestania activa se
  dibuja mas alta y mas ancha que las demas y tapa el borde superior de la
  pagina, de modo que las dos partes se leen como una sola hoja: eso es lo que
  distingue una pestania de un boton. El rect del widget cubre la fila Y la
  pagina; la app pregunta por `sxgui_tabs_page()` donde dibujar su contenido y
  por `sxgui_tabs_preferred_width()` cuanto mide la fila. Con el foco puesto,
  las flechas cambian de pestania. El selector de grupos del Program Manager
  --  que hasta ahora eran cajas con bisel pintadas a mano, y se leian como una
  fila de botones --  pasa a usarlo.

- **Barra de tareas al pie de la pantalla, con las ventanas abiertas.** Es un
  CLIENTE del WM (`/bin/taskbar`) y no chrome de windowd -- el modelo de
  explorer.exe, y el mismo layering que separo a shellui y progman del window
  manager. Solo lista ventanas: sin menu inicio y sin area de notificaciones.
  Cada boton lleva icono y titulo, el de la ventana activa va hundido, y un
  click activa o -- si ya estaba activa -- minimiza, como en Win95. Los botones
  van en orden ESTABLE por slot y no por z-order: si se reordenaran al cambiar
  de ventana, el click siguiente caeria sobre otra cosa. Lo que un
  cliente no puede saber (que ventanas hay, cual esta activa, cual minimizada)
  se lo cuenta el WM por los canales nuevos de `savanxp/wm_shell_protocol.h`:
  la lista por una seccion compartida con seqlock, los clicks de vuelta por un
  pipe. Esos dos descriptores se cablean SOLO al cliente del rol, asi que no
  mueven el techo de ventanas simultaneas. **Ni el fondo ni una app a pantalla
  completa saben que existe**: la barra queda detras de una app fullscreen, y
  lo unico que le reserva lugar es maximizar una ventana.

- **`tools/shoot.ps1`: verificacion visual de la sesion, headless.** Arranca el
  sistema sin ventana, le manda teclas y saca capturas PNG. Existe porque los
  harnesses asertan estado y geometria pero no apariencia -- pasan en verde con
  la pantalla mal, que es exactamente como se escapo el bug de Ctrl+C. Las
  teclas van por QMP y no por el `sendkey` del monitor, porque hace falta
  SOSTENER un modificador: sin eso no se puede capturar el switcher de Alt+Tab
  abierto ni hacer un Ctrl+C. Trae los escenarios `desktop`, `alttab` y
  `clipboard` y `taskbar`; este ultimo no solo captura sino que **verifica
  pixeles** y esta cableado como `build.ps1 taskbar-smoke`. Falla temprano si
  hay un spec de automatizacion
  plantado, que haria arrancar el ultimo harness en vez del escritorio. Tambien
  maneja el mouse: el del guest es un PS/2 relativo, asi que el cursor se lleva
  primero contra la esquina -- el clamp del WM lo deja en (0,0), el unico origen
  conocido -- y despues se avanza de a 64 px, porque un salto grande se parte en
  una rafaga de paquetes que el guest no alcanza a drenar. No es parte del
  build.
- **Alt+Tab para cambiar de ventana.** Mientras Alt sigue apretado cada Tab
  mueve la seleccion y al soltarlo se confirma, con Alt+Shift+Tab yendo al
  reves. El switcher que se muestra es el Task List, que ya enumeraba lo mismo:
  no hay UI nueva. El ciclo arranca en la ventana activa, asi que el primer Tab
  cae en la ultima usada. Segundo item de la Fase C del WM, despues del resize
  por bordes.
- **Seleccion de texto en sxgui, con Cortar, Copiar y Pegar.** Los widgets de
  texto tenian caret pero no ancla, asi que no habia de donde copiar. Ahora
  shift junto con flechas, Inicio, Fin y las de pagina extiende la seleccion;
  el click ancla y el arrastre la estira; shift+click extiende desde donde
  estaba, y lo seleccionado se pinta en video inverso. Escribir, Enter,
  Backspace y Delete sobre una seleccion la reemplazan. Atajos Ctrl+C, Ctrl+X,
  Ctrl+V y Ctrl+A; el resto de los Ctrl+letra se consumen en vez de escribirse,
  salvo con AltGr, que en varias distribuciones llega como Ctrl+Alt. Vale para
  el editor multilinea y para el campo de una linea -- que es donde vive el
  dialogo Abrir/Guardar, o sea el lugar donde mas natural es pegar una ruta --,
  con la unica diferencia de que ahi un pegado con saltos se queda con la
  primera linea. Las operaciones se exponen ademas como `sxgui_textedit_copy`,
  `_cut`, `_paste`, `_select_all` y `_has_selection` para poder colgarlas de un
  menu, y el bloc de notas estrena su menu Edit. Lo cubre `seltest`, dentro de
  `build.ps1 smoke`, que maneja el toolkit headless contra un buffer de
  pixeles.
- **Portapapeles del sistema, en `/dev/clipboard`.** Copiar y pegar entre
  programas, que hasta ahora no existia en ninguna forma. El contenido es un
  valor y no un stream: un `write` reemplaza todo y un `read` devuelve desde el
  principio, sin cursor, asi que leer dos veces da lo mismo. Pasarse de
  `SAVANXP_CLIPBOARD_CAPACITY` (8 KiB) falla con `ENOSPC` en vez de truncar en
  silencio. `CLIP_IOC_GET_INFO` devuelve largo, formato y una secuencia que
  sube con cada cambio, para que un menu pueda saber si rehabilitar Pegar sin
  releer el contenido entero. La SDK lo envuelve en `clipboard_set_text`,
  `clipboard_get_text`, `clipboard_get_info` y `clipboard_clear`, que abren y
  cierran el device en cada llamada y no cuestan un descriptor permanente. Lo
  cubre `cliptest`, dentro de `build.ps1 smoke`.

### Cambiado

- **`icon=` en `progman.ini` apunta a un programa, no a un catalogo de arte.**
  El valor era un nombre que se resolvia contra un array de pixeles horneado
  (`icon_id_from_name()`, que se borra entera); ahora guarda un PATH y el icono
  se lee del binario apuntado, con el MISMO mecanismo que el item ya usaba para
  el suyo -- pedir el icono de OTRO programa es justamente el caso de uso, por
  ejemplo una segunda entrada del mismo ejecutable con otro nombre. Los nombres
  viejos (`shell`, `notepad`, `gfxdemo`, `keytest`, `mousetest`) siguen andando
  como alias, resueltos al path del programa que hoy dibuja ese icono; ya no
  hace falta escribirlos en un `.ini` nuevo, porque esos programas traen su
  propio `.sxicon`. Un `icon=` explicito le gana al binario aunque tenga icono
  propio: `icon=desktop` en un item que lanza el notepad fuerza el generico y
  ni siquiera intenta leer su `.sxicon` real, porque el usuario dijo "no" a
  proposito y eso pesa mas que lo que declare el ejecutable.

- **El chrome de sxgui pasa al bisel de dos pixeles de Win95.** Un borde 3D de
  la epoca lleva cuatro tonos -- se agrego `SXGUI_COLOR_BEVEL` (223,223,223),
  el que faltaba -- repartidos en un anillo externo y uno interno, y el hundido
  es ahora el reverso exacto del levantado, de modo que un boton apretado y una
  caja de texto tienen el mismo espesor. Alcanza a botones, campos, listas,
  editores, cabeceras de columna, popups de menu y dialogos. Ademas:
  rectangulo de foco punteado en vez de un marco lleno que competia con el
  borde, canal de las barras de scroll con la trama al 50%, tilde del checkbox
  redibujado para que entre en su caja de 13, radio button con anillos
  concentricos partidos por la diagonal -- antes era una mancha blanca sin
  borde visible --, texto grabado en los controles deshabilitados y barra de
  menu sin la linea separadora, que el original no tiene.

- **Las apps de sxgui comparten una sola grilla de layout.** Las metricas viven
  en `savanxp/sxgui.h` (`SXGUI_MARGIN`, `SXGUI_GAP`, `SXGUI_BUTTON_WIDTH`,
  `SXGUI_FIELD_HEIGHT`, `SXGUI_STATUS_HEIGHT`, `SXGUI_DIALOG_MARGIN`,
  `SXGUI_BORDER_*`, `SXGUI_SCROLLBAR_THICKNESS`) y las usan notepad, files,
  progman, aboutapp y widgetsdemo. Antes cada ventana tenia su propio margen
  -- 8 a los costados y 5 arriba y abajo, con botones de tres anchos
  distintos -- y se veian bien por separado y desparejas juntas. Las filas de
  botones de dialogo pasan a ir pegadas a la derecha cuando el dialogo pide un
  dato y centradas cuando hace una pregunta. En files, la barra de direccion y
  el boton "Up One Level" ahora miden lo mismo y quedan alineados.

- **`tools/shoot.ps1` suma el escenario `files`.** Abre el explorador y mueve
  la seleccion: es la ventana con mas controles distintos a la vez -- barra de
  direccion, boton, lista de detalles con cabecera y barra de estado de dos
  paneles --, asi que es donde se ve si un margen o una sangria del toolkit se
  fue.

- **El evento de teclado lleva los modificadores.** `savanxp_input_event` suma
  el campo `modifiers` con el estado de Shift, Ctrl, Alt, AltGr y los locks en
  el instante del evento (`SAVANXP_KEY_MOD_*`). El kernel ya lo calculaba para
  su uso interno -- es lo que mueve el Ctrl+L de la consola -- y lo descartaba
  al cruzar a userland, asi que el WM tenia que seguir Ctrl a mano por los
  KEY_DOWN/KEY_UP de la tecla; ese seguimiento quedaba trabado si se perdia un
  KEY_UP, por ejemplo al soltar Ctrl con el foco ya en otra ventana.
  `include/kernel/input.hpp` deriva sus flags de los del SDK para que las dos
  listas no puedan divergir. El evento pasa de 12 a 16 bytes: **las apps
  externas compiladas contra la SDK anterior necesitan rebuild** -- Doom lee
  este struct.

### Eliminado

- **El set de iconos horneado en `desktop_icons.h` queda en uno solo.** Doom
  primero, y despues Shell, Notepad, Gfx Demo, Key Test y Mouse Test, salen del
  enum y de las dos tablas de bitmaps: lo unico que sobrevive es
  `DESKTOP_ICON_DESKTOP`, que es la red de seguridad para cuando un binario no
  se puede leer en absoluto y no una opcion mas entre varias. Los mismos
  pixeles de siempre ahora salen unicamente del `.sxicon` de cada programa --
  el de Doom vive en `sdk/doomgeneric/icon.png` y su manifiesto lo declara con
  `icon_file=`. Los PNG de `assets/desktop/icons/` NO se van: siguen siendo el
  catalogo que cada `.sxres` referencia con `icon=<nombre>`, y
  `gen_desktop_source_art.py` los sigue regenerando. Eran dos catalogos que
  compartian arte por casualidad -- uno horneaba a un header C y el otro a un
  `.sxicon` --, no el mismo catalogo con dos nombres.

- **Borrado el arte de la franja del menu inicio.** El menu se retiro con el
  resto del chrome Win95, pero la cadena que lo dibujaba seguia entera:
  `gen_desktop_source_art.py` generaba `menu_strip_savanxp.png`,
  `gen_desktop_icon_assets.py` lo horneaba a C y `desktop_icons.c` lo envolvia
  en un simbolo que no usaba nadie -- con su warning de variable sin uso en
  cada build.

### Corregido

- **Un volumen SVFS2 montado en solo-lectura volvia a ser escribible al
  publicarse.** `svfs::attach()` fijaba el status en `mounted` sin mirar como
  habia quedado el montaje, asi que un volumen cuyo journal no se pudo recuperar
  -- `recover_journal()` no logro persistir el replay -- terminaba aceptando
  escrituras sobre metadata on-disk nunca reconciliada. El driver quedaba
  partido al medio: los vnodes ya estaban publicados con `writable=false`
  mientras `svfs::writable()` decia que si. Ahora `attach()` respeta el
  `read_only` que dejo `probe()`. Se suma `build.ps1 svfs-smoke`, un test de
  host (`tests/host/`) que linkea el driver real contra block/vfs de mentira y
  una imagen armada con libsvfs: es la unica forma de montar un volumen roto,
  porque el kernel no expone `mount` a userland.

- **Dos bugs de windowd que destapo el popup del selector de teclado.** El
  click-down caia en el camino de las ventanas overlay -- el mismo bug ya
  resuelto para la barra de tareas, que no se habia replicado aca -- y el popup
  nunca entraba en las listas de senializacion de composed/retire, asi que su
  segundo `gfx_present()` se colgaba esperando un retire que no iba a llegar
  nunca.

- **El generador de recursos SXE borroneaba un icono al agrandarlo.**
  `collect_icons_from_file()` solo detectaba el multiplo entero para ACHICAR
  (`source % target == 0`), no para agrandar: un original de 16x16 -- el caso
  mas comun del pixel art, dibujado una vez al tamano chico -- derivaba su
  32x32 con LANCZOS y salia borroneado en vez de nitido con NEAREST. Nadie lo
  habia ejercitado, porque el unico manifiesto con arte propio usaba un source
  de 48x48 que achica limpio. Aparecio al comparar el blob ESTAMPADO byte a
  byte contra el PNG original, que es justo la clase de bug que un round-trip
  existe para cazar y que un test de "genero un `.sxicon`" no ve.

- **La columna Type de files quedaba cortada al aparecer la barra de scroll.**
  El reparto del ancho entre columnas descontaba 6 pixeles fijos, pero el area
  util de la lista son los dos biseles hundidos mas los 16 de la barra cuando
  esta. Ahora la barra se reserva siempre, asi que las columnas tampoco saltan
  de ancho al agregarse un archivo.

- **El texto de las filas no quedaba a plomo con el rotulo de su columna.** El
  bisel de la celda de cabecera come dos pixeles que la fila no descontaba.

- **En widgetsdemo la barra de scroll suelta se superponia con la segunda
  columna.** El origen de la columna se calculaba desde el ancho de la lista
  sin contar la barra que va entre las dos.

- **Apretar Ctrl soltaba la seleccion, asi que Ctrl+C no copiaba nada.** Una
  tecla modificadora produce su propio evento de teclado, con `ascii` en cero, y
  los widgets de texto tratan "cualquier otra tecla" como motivo para soltar la
  seleccion: para cuando llegaba la letra del atajo ya no habia nada
  seleccionado. Ahora las modificadoras (Ctrl, Alt, AltGr y los locks) no
  llegan a los widgets. Solo se veia en vivo -- el test inyectaba el acorde ya
  formado, sin el evento de la tecla modificadora que manda un teclado real, y
  por eso pasaba.

## [0.3.4] - 2026-08-28

### Agregado

- **El malloc de userland ya no vive en una arena fija de la BSS: crece
  pidiendole secciones al kernel.** La arena unica que se reservaba en la BSS
  era RAM fisica residente por proceso desde el exec -- el kernel mapea la BSS
  entera -- aunque la app no tocara un byte, y cada app tenia que adivinar de
  antemano cuanto iba a necesitar. Ahora queda un bootstrap chico de 256 KiB en
  la BSS y, cuando algo no entra, el allocator pide arenas con `section_create`
  + `map_view` y las devuelve con `unmap_view` en cuanto quedan enteras libres.

  Las vistas se mapean PRIVATE a proposito: asi `clone_address_space` clona la
  seccion con su contenido en el fork, que es la misma semantica que daba la
  BSS. Las arenas crecen geometricamente porque son un recurso escaso -- el
  kernel da 32 section views por address space, compartidos con las superficies
  de GPU/WM, y 64 section objects en todo el sistema --, y el piso de la proxima
  se calcula sobre los bytes mapeados **hoy**, no sobre un contador monotono,
  para que un ciclo alocar/liberar no termine pidiendo el maximo cada vuelta.

  Las apps externas del SDK dejan de forzar `-DSX_HEAP_SIZE`: `-HeapMiB 0` es el
  nuevo default y significa dinamico. Doom dejo de clavar 24 MiB de arena y su
  BSS bajo de ~25 MB a 642 KB.

  Lo cubre `heaptest`, dentro de `build.ps1 smoke`: valida el crecimiento, que
  lo que vive en el bootstrap no se mueva, el reciclado de arenas (falla si se
  filtran), varias arenas vivas a la vez, el realloc que cruza arenas y que el
  fork se lleve una copia privada.

  Ojo con la consecuencia: desde este cambio un `malloc` normal puede devolver
  direcciones arriba de 4 GiB, asi que cualquier driver que trunque un puntero
  de userland a 32 bits pasa a ser un bug vivo.

- **La apertura de VRAM entera se mapea write-combining.** El firmware mapea
  solo el modo visible (4000 KiB de los 16 MiB que hay), y eso era el techo de
  todo: sin mas memoria mapeada el doble buffer solo entraba en el modo bajo de
  fullscreen. Ahora `fb_gpu` pregunta al propio dispi cuanta VRAM hay y mapea la
  apertura completa, asi que **el doble buffer tambien anda en el escritorio
  nativo**.

  El tipo de memoria se elige buscando en `IA32_PAT` que indice quedo
  configurado como write-combining, en vez de asumir el layout: el bootloader
  ya deja PAT armado y no hace falta tocar el MSR. Si ninguna entrada es WC se
  copia el tipo que el firmware le puso al scanout. Write-combining es lo que
  corresponde a un framebuffer -- escrituras en rafaga que nadie relee.

  Antes de confiar en el mapeo nuevo se verifica que sea de verdad la misma
  memoria: se escribe un patron por una vista y se lee por la otra (con
  `sfence`, porque con write-combining la escritura puede quedar en un buffer).
  Si no coinciden, la direccion fisica deducida estaba mal y se descarta el
  mapeo en vez de escribir en cualquier lado.

  API nueva en `vm::`: `kPagePat`, `map_kernel_device_memory` (como
  `map_kernel_mmio` pero sin forzar cache-disable, que para un framebuffer lo
  volveria inutilizablemente lento), `kernel_page_cache_flags` y
  `write_combining_page_flags`. `map_kernel_mmio` pasa a ser una envoltura.

  **Bug corregido de paso**: leer los bits de cacheo de un mapeo exige
  distinguir paginas grandes. En una pagina de 2 MiB el bit 7 es PS, no PAT --
  el bit PAT es el 12 --, y el framebuffer de Limine viene mapeado justamente
  asi. Interpretar PS como PAT hacia leer un tipo de memoria equivocado.

- **Doble buffer por panning en el framebuffer plano (sin tearing).** Con alto
  virtual del doble que el visible, la VRAM guarda dos frames y el registro
  `Y_OFFSET` de dispi elige cual se muestra. El compositor compone siempre
  sobre el que NO esta a la vista y despues se flipea, asi el host nunca
  escanea un buffer a medio escribir. El flip es una sola escritura de
  registro, o sea atomico para el que escanea.

  Se activa solo si los dos buffers entran en lo que hay **mapeado**. Con la
  apertura de VRAM entera mapeada (ver arriba) eso incluye el escritorio
  nativo; sin ella habria quedado limitado al modo bajo de fullscreen.

  Antes de confiar en el panning se lo prueba: se escribe `Y_OFFSET` y se
  relee. Un dispositivo que acepte el alto virtual pero ignore el registro
  dejaria el flip sin efecto y la pantalla congelada en un buffer, que es peor
  que el tearing; si el probe falla se queda en un solo buffer.

  **Todos** los presents flipean, tambien los de dano parcial: el buffer sobre
  el que se compone tiene el contenido de hace DOS frames, asi que se le
  reaplica el dano del frame anterior ademas del actual. La union de los dos
  alcanza, porque un pixel que difiera entre hace dos frames y ahora tuvo que
  cambiar en alguno de los dos. Cuesta ~2x el area danada, nada comparado con
  copiar la superficie entera (4 MiB por mover el cursor), asi que el
  escritorio conserva su optimizacion de dano y ademas queda sin tearing. Se
  cae a copia completa solo cuando el destino esta realmente desactualizado:
  primer frame, present de superficie completa, o cambio de superficie. Los
  caminos crudos (`present`/`present_region`, que usan la consola y gputest)
  tocan los buffers por fuera del seguimiento, asi que lo invalidan. Al soltarse
  la sesion grafica el doble buffer se apaga y `Y_OFFSET` vuelve a 0, porque la
  consola escribe al inicio del scanout y no sabe de paginas alternas.

  **Pedir el modo que ya esta puesto no es un no-op la primera vez.** El que
  dejo el firmware no tiene el alto virtual que necesita el doble buffer, y el
  compositor arranca pidiendo exactamente la resolucion nativa: sin programarlo
  igual, el escritorio nunca lo activaba y el doble buffer terminaba siendo
  solo para fullscreen.

  `boot::FramebufferInfo` gana `mapped_bytes`: los bytes que el mapa de memoria
  dice que hay detras del scanout. Reemplaza al modo nativo como techo para
  cambiar de modo, y es lo que decide si el doble buffer entra.

- **El backend de framebuffer plano puede cambiar de modo (VBE de Bochs).**
  Hasta ahora `fb_gpu` aceptaba unicamente la resolucion que dejaba el
  firmware: `set_mode` rechazaba cualquier otra cosa y el conector no anunciaba
  `MUTABLE_MODE_SETTING`. Ahora detecta la interfaz dispi del "Bochs Graphics
  Adaptor" -- los puertos 0x1CE/0x1CF que implementan tanto la VGA estandar de
  QEMU como VBoxVGA -- y programa el modo de verdad, releyendo del dispositivo
  la geometria que quedo en vez de confiar en la pedida (el ancho virtual, o
  sea el pitch, lo puede redondear la implementacion). Es la palanca que le
  faltaba al camino sin virtio-gpu, que ademas es el hardware por defecto de
  `build.ps1`. Nuevo log de boot: `fb_gpu: <W>x<H> nativo, mode-setting ...`.

  Limites y garantias: el techo es el modo nativo, porque el unico mapeo de
  scanout que existe es el que armo el firmware y uno mas grande escribiria
  fuera (subir de ahi, y el doble buffer por panning, piden mapear antes la
  apertura de VRAM entera). Cambiar de modo exige la sesion grafica y que no
  haya superficies importadas vivas, mismo contrato que virtio-gpu. Si la
  programacion falla se repone el modo anterior en vez de quedar en uno a
  medio armar. Al soltarse la sesion grafica se vuelve al nativo, asi que una
  app que se muere en modo bajo no deja la pantalla ahi. `GET_SCANOUTS` y las
  propiedades del conector ahora distinguen la resolucion nativa de la activa,
  que antes eran la misma por construccion.

  Cobertura: el subtest de `SET_MODE` de `gputest` estaba escrito pero se
  salteaba solo, porque esta condicionado a `MUTABLE_MODE_SETTING`. Con el flag
  anunciado, `build.ps1 gpu-soak` ejercita el ciclo 640x400 -> nativo con
  presents en el medio tambien sobre el framebuffer plano, sin harness nuevo.
  Falta la prueba en VirtualBox real: expone dispi, pero como se lleva con el
  modo que deja Limine todavia no se verifico.

- **Formato SXE: los ejecutables traen adentro su identidad.** Un manifiesto
  `<nombre>.sxres` al lado del fuente se estampa en secciones no-alloc del ELF
  (`.sxmeta`/`.sxicon`), asi que el binario declara solo su titulo, version,
  iconos de 16x16 y 32x32, color de barra de titulo y las extensiones que sabe
  abrir. Program Manager toma de ahi nombre e iconos, `windowd` el titulo, el
  icono y el accent de la ventana, y Files abre cada archivo con el programa
  asociado en vez de mandar todo al bloc de notas. Las asociaciones combinan
  `/disk/assoc.ini` (politica del usuario) con lo que declaran los binarios
  instalados, con la politica arriba. Antes la identidad de cada programa vivia
  en tablas por path adentro de progman y del WM, que habia que editar y
  recompilar para agregar una app. Formato en `docs/SXE_FORMAT.md`; targets
  nuevos `build.ps1 sxe-smoke` y `build.ps1 filesapp-smoke`.

- **Boton por defecto en los dialogos** (`default_button` en
  `struct sxgui_dialog`). Enter lo dispara y se dibuja con el borde doble de
  la epoca, asi que se ve cual responde antes de apretarlo; si el foco esta
  sobre otro boton, gana ese. Declarado en los dialogos del sistema: Save del
  notepad, OK de los About y **No** en el de apagar, donde el default tiene
  que ser la opcion que no hace nada.

- **El notepad avisa antes de perder cambios sin guardar.** Exit, New y Open
  preguntan Save / Discard / Cancel si el documento esta modificado, y retoman
  la accion despues de guardar. Solo cubre las salidas por la app: cerrar por
  la X de la ventana sigue matando el proceso sin preguntar, porque el WM
  manda SIGKILL en vez de pedir el cierre.

- **Bloc de notas (`/bin/notepad`).** Editor de texto con la forma del de la
  era Win95: menu File/Edit/Search/Help, area de texto completa y barra de
  estado, que muestra el archivo abierto y un asterisco si tiene cambios sin
  guardar (la barra de titulo la pone el WM y la app no puede tocarla). Abre y
  guarda archivos (New, Open..., Save, Save As...; F2 guarda, F3 abre). Es la primera app del SO que escribe archivos. Limite de 32 KB por
  documento, avisado en la barra de estado al abrir algo mas grande.

- **Editor multilinea en sxgui (`sxgui_textedit`).** Widget nuevo con caret,
  insercion y borrado, Enter que parte la linea, flechas que conservan la
  columna, Home/End/PageUp/PageDown, click para posicionar el caret y scroll
  vertical con scrollbar. Sin word wrap: las lineas largas siguen al caret con
  scroll horizontal.

- **Un launch puede llevar un argumento.** `savanxp_desktop_launch_request`
  suma un campo `argument` que el WM pasa como `argv[1]` del programa lanzado,
  expuesto en el SDK como `gfx_desktop_launch_arg()`. Es lo que permite "abri
  este archivo": Files lanza el notepad con la ruta.

- **Los dialogos pueden arrancar con el foco en un widget** (`initial_focus` en
  `struct sxgui_dialog`). Sin esto un dialogo de entrada de texto abria sin
  foco y el teclado no llegaba a ningun lado.

- **Listbox con columnas en sxgui (vista de detalles).** Poniendo `columns` en
  el widget, el listbox dibuja una cabecera fija arriba y parte cada item por
  TAB, una celda por columna, con alineacion a la derecha opcional
  (`SXGUI_COLUMN_RIGHT`) y recorte por celda. Es opt-in: sin `columns` el
  listbox se comporta igual que siempre.

- **Size hint: cada ventana arranca del tamano de su contenido.** Canal nuevo
  cliente->WM (`SAVANXP_WM_FD_SIZE_HINT`, fd 11): la app pide el area util que
  necesita y el WM la aplica una sola vez al arrancar, recortada a la capacidad
  de la superficie y recolocando la ventana; despues manda el usuario. SDK
  posix: `gfx_request_content_size()` / `gfx_wait_content_size()`, espejado en
  el SDK nativo. En sxgui el ajuste es opt-in: `sxgui_app_autosize()` (bounding
  box de los widgets) o `sxgui_app_set_content_size()`. Antes toda app recibia
  la misma superficie generica y abria con el contenido en una esquina.
- **`svfs-cli rm`: se puede sacar algo de una imagen SVFS2.**
  `svfs-cli rm <imagen> <ruta>...`, con la convencion de rutas del manifiesto.
  Solo archivos y directorios vacios; no crea los padres que falten y no borra
  la raiz. Antes el sync era puramente aditivo y la unica forma de sacar un
  binario era recrear la imagen entera.

- **Redimensionar ventanas por los bordes.** Los bordes y las esquinas del
  marco arrastran, y el cursor los anticipa (`RESIZE_H`/`RESIZE_V`). El borde
  opuesto queda anclado, asi que tirar del izquierdo o del superior mueve la
  ventana mientras cambia de tamano, y el minimo topa en vez de empezar a
  desplazarla. No aplica a ventanas sin marco, maximizadas ni en fullscreen.
- **Cursores multi-forma estilo Win9x.** Ocho formas (`arrow`, `wait`, `text`,
  `move`, `resize-h`, `resize-v`, `unavailable`, `link`) en vez del arrow unico
  fijo. Las apps piden la suya por el canal de hint del WM
  (`savanxp_desktop_cursor_hint`) y el WM la resuelve por prioridad contra lo
  que este pasando: arrastre de ventana, borde de resize o app ocupada.
- **Wallpaper de fabrica en `/disk/wallpaper.bmp`.** La imagen va commiteada en
  `diskfs/` y se genera con `tools/GenerateDefaultWallpaper.py` con seed fija,
  asi que el build no depende de Python para tenerla. El fondo se cambia desde
  Options del Program Manager, que reescribe `/disk/desktop.cfg`.
- **`build.ps1 net-smoke`: cobertura automatizada del NIC.** Valida presencia
  por PCI, MAC propia distinta de cero, direccion y gateway configurados, y
  hace ARP + ICMP contra el gateway de slirp exigiendo que los contadores de
  tx/rx avancen. Antes la red no tenia ningun harness: el unico rastro era el
  `net online` del handoff, que solo dice que `initialize()` no fallo.
- **`build.ps1 build -NoTestApps`: imagen sin las apps de diagnostico.** Los
  programas marcados como de test (keytest, gfxdemo, smoke, audiotest, ...) no
  entran al rootfs (59 -> 32 binarios) y el launcher se compila sin sus
  entradas. Los comandos de automatizacion las incluyen siempre, porque sus
  harnesses dependen de ellas.

### Cambiado

- **F11 baja la resolucion del scanout en vez de escalar por software.** Una
  app fullscreen-capable rinde a 640x400 y hasta ahora el shell estiraba ese
  buffer hasta el framebuffer en cada frame, con un escalador que hace dos
  divisiones enteras por pixel de salida: mas de un millon de divisiones por
  frame a 1280x800. Ahora, al entrar en fullscreen, el shell le pide al
  compositor el modo de la superficie del cliente; con el scanout en 640x400
  los pixeles van 1:1 y el escalado desaparece. Es lo que estaba esperando el
  mode-setting del backend plano, y sobre virtio-gpu funciona igual. Si el
  adaptador no sabe cambiar de modo se compone escalado como antes: es
  degradacion, no error.

  Mecanica: mensaje nuevo `SAVANXP_COMPOSITOR_MSG_SET_MODE` en el protocolo del
  compositor. El daemon suelta la superficie importada (apunta al modo viejo),
  reprograma y reimporta la misma seccion contra la geometria nueva -- la
  seccion esta dimensionada para el modo mas grande, asi que un modo menor
  entra sin reasignar nada y el puntero al backbuffer del shell no se mueve. Si
  algo falla repone el modo anterior, para no dejar al shell sin scanout. Del
  lado del shell alcanza con actualizar `gfx.info`: `windowd_render` envuelve
  el backbuffer con esa geometria en cada frame.

  Tres caminos de vuelta, porque el modo es del scanout y no del cliente:
  salir de fullscreen, que la app fullscreen se muera (End Task o sola), y que
  se muera el compositor -- que respawnea siempre en el nativo, asi que el
  shell le vuelve a pedir su modo o compondria contra una geometria que el
  scanout ya no tiene. El cursor se reencuadra si el modo nuevo lo dejo fuera
  de pantalla.

  El smoke ahora **verifica** el cambio: entrar en fullscreen tiene que dejar
  `gfx.info` en 640x400 y salir tiene que devolverlo al nativo. Suma una
  segunda caida inyectada del daemon, esta vez con el scanout en modo bajo, y
  exige que se haya disparado. Para que sea observable hubo que generar dano
  por iteracion durante el fullscreen: sin dano el shell no presenta, y sin
  presentar nunca se entera de que el daemon murio, porque la deteccion es por
  fallo de present.

- **La geometria cacheada del kernel se repone en cada cambio de modo.**
  `GPU_IOC_SET_MODE` es el unico punto por donde userland muta el modo, asi que
  ahi se llama a `ui::sync_framebuffer_geometry()`, que relee del backend y
  reprograma la extension del puntero absoluto (tablet virtio). Con la
  extension vieja el cursor apuntaria a otro lado despues de un cambio de modo;
  era latente desde siempre y con F11 cambiando de modo pasa a ser rutina.

- **`memcpy`/`memset` dejan de mover la memoria de a un byte.** Las tres
  implementaciones del arbol -- kernel (`kernel/runtime.cpp`), SDK posix
  (`libc.c`) y SDK nativo (`sx_native.c`) -- eran lazos en C de un byte por
  iteracion, y como el arbol se compila sin `-O` eso costaba media docena de
  instrucciones por byte. Ahora usan instrucciones de string (`rep movsq` /
  `rep stosq` mas la cola en bytes), que mueven 8 bytes por iteracion sin
  overhead de lazo; 8 es el maximo disponible porque todo se compila con
  `-mgeneral-regs-only`. Pega en todo el camino de pixeles: el blit por filas
  del compositor (`sx_painter_blit_bitmap`) y el volcado al scanout de
  `fb_gpu`, que con VGA estandar -- el hardware por defecto -- arrastra los
  4 MiB de un frame de 1280x800 en cada present completo. `windowd-smoke`, que
  corre una cantidad fija de frames, baja de ~37 s a ~30 s de punta a punta
  (con el boot y el armado de imagen adentro, que no cambian).
- **`cld` en cada entrada al kernel y en el arranque de cada proceso.** DF es
  parte del RFLAGS del proceso, asi que un programa podia entrar al kernel con
  la bandera de direccion prendida y hacer que las instrucciones de string
  caminaran hacia atras. Se limpia en los stubs de `context.S`, en las macros
  `DEFINE_ISR_*` de `cpu_init.cpp` (el atributo `interrupt` no lo emite solo) y
  en el `crt0`. Es la precondicion de las rutinas de memoria nuevas, y tambien
  lo que hace seguro prender `-O2` mas adelante: con optimizacion clang emite
  `rep movsb` por su cuenta para copias de structs.
- **Los blits de rectangulo completo se copian de una sola pasada.**
  `fb_gpu::blit_rect` y `sx_painter_blit_bitmap` emitian una llamada a `memcpy`
  por fila aunque el rectangulo cubriera el ancho completo y ningun pitch
  tuviera padding, o sea aunque las filas ya vinieran contiguas en origen y
  destino. Cuando se da ese caso -- `present()` a pantalla completa, el batch
  con `FULL_SURFACE`, el blit de una superficie entera -- ahora va una sola
  copia lineal. Los rects sucios chicos siguen por el camino de a filas.

- **Files abre los archivos en el bloc de notas.** Activar algo que no es un
  programa ya no responde "No application is associated with this file": lanza
  `/bin/notepad` con la ruta. El preview que Files tenia adentro tiene casa.

- **Files toma la forma del explorador de archivos de la era Win95.** Barra de
  direccion con "Up One Level" (apagado en la raiz), lista de detalles con
  cabecera Name/Size/Type -- tamanos en KB, tipos "File Folder"/"Application"/
  "TXT File" -- y barra de estado de dos paneles con el conteo de objetos y el
  total. El menu pasa a File/View/Help.

- **Files ya no muestra el contenido de los archivos.** Se fue el panel de
  preview: leer un archivo es trabajo de un editor y va a vivir en un notepad
  propio. Abrir algo que no se puede lanzar ahora lo dice en la barra de
  estado ("No application is associated with this file.") en vez de volcar los
  primeros bytes.

- **Display, audio, block y NIC eligen driver por registro, no por ramas en
  `kernel_main`.** Cada driver se auto-describe con `register_driver` y el HAL
  lo ata por prioridad: `bind_best` para display (virtio-gpu 100 / framebuffer
  10), audio (virtio-sound 100 / ac97 50) y NIC; `probe_all` para block, cuyos
  devices coexisten (ata 100 antes que ramdisk 10, que es lo que hace que un
  disco IDE le gane a la imagen del LiveCD). Sumar un backend ya no toca el
  flujo de arranque. `kernel/ata.cpp`, `ramdisk.cpp` y `rtl8139.cpp` salen a
  unidades propias y `net.cpp` queda como stack L3/L4 puro detras de la vtable
  `nic::Nic`. Cambios de API: `block::initialize` -> `block::probe_all`,
  `block::register_ramdisk` -> `ramdisk::attach_image`. Nuevos logs de boot:
  `display: backend`, `audio: backend`, `nic: driver`, `block: N device(s)`.
- **Assets del escritorio: de System.Drawing (GDI+) a Pillow.** Los tres
  generadores pasan a Python + Pillow (`tools/gen_cursor_asset.py`,
  `gen_desktop_icon_assets.py`, `gen_desktop_source_art.py`), con el mismo
  header C generado y sin cambios de ABI. Nuevo requisito de build en cualquier
  plataforma: `python3` + Pillow, fuera del toolchain horneado.
- **Migracion del build a Linux: rutas y herramientas del host.** Los builds
  "aparte" usan `/` en vez de `\` literal (`Join-Path` no separa por `\` fuera
  de Windows); `Build-Iso` deja de forzar rutas cygdrive segun `Test-IsWindowsHost`
  y compila el deployer `limine` con `make` cuando no hay binario prebuildeado.
  Nuevo requisito en Linux/macOS: `make` + `cc`. Sin cambios en Windows.
- **El Program Manager ya no lista programas que no estan instalados.** Nuevo
  `progman_registry_prune_missing(exists)`: descarta los items cuyo path no se
  puede abrir y los grupos que quedan vacios, igual para los defaults horneados
  que para `/disk/progman.ini`. Puede dejar el registro vacio: si no hay nada
  lanzable, mostrar nada es lo honesto. `windowd_appinfo.c` no se filtra, porque
  traduce path -> nombre/icono de ventanas ya abiertas, no de accesos directos.
- **El build ya no regenera el arte del desktop en cada compilacion si nada
  cambio.** `Generate-CursorAsset`/`Generate-DesktopIconAssets` corrian los tres
  generadores Pillow sin condicion; con una version de Pillow distinta a la que
  produjo los PNG commiteados, cada build dejaba diffs binarios espurios en
  `assets/desktop/` (mismos pixeles, distinta compresion). Ahora comparan
  mtimes contra el script y los PNG fuente, igual criterio que ya usaba
  `Build-SvfsCli`, y solo regeneran si algo cambio de verdad.

- **QEMU se arma con hardware "base" por defecto.** VGA estandar, mouse PS/2 y
  audio AC'97 -- el mismo hardware que emula VirtualBox --, para ejercitar los
  backends de fallback (`fb_gpu`, `ps2`, `ac97`) sin salir de QEMU. El switch
  `-Virtio` vuelve a virtio-vga + virtio-tablet + virtio-sound y vale para
  `run`, `debug` y todos los harnesses. Antes la maquina era virtio siempre y
  los caminos de fallback solo se probaban arrancando VirtualBox a mano.
- **El window manager se separo del shell (modelo NT 3.5).** El proceso que
  antes era `desktop` hacia dos cosas soldadas: manejaba las ventanas y
  dibujaba el chrome Win95. Ahora el WM es `windowd` (`/bin/windowd`) y el
  shell son procesos cliente: `shellui` dibuja el fondo y `progman` es el
  launcher, con sus grupos e items en `/disk/progman.ini`, editable sin
  recompilar el SO. Se retiraron taskbar, menu de inicio, iconos del
  escritorio y menu contextual; su funcion la cubren el **Task List**
  (Ctrl+Esc), que es UI del WM y lista las ventanas incluidas las minimizadas,
  y el menu File de progman, donde ahora viven Apagar y Reiniciar. El contrato
  WM<->cliente quedo en el header `savanxp/wm_protocol.h` (fds 3..11), antes
  implicito y repartido en numeros crudos. El target `desktop-smoke` pasa a
  llamarse `windowd-smoke`.
- **SVFS2 tiene una sola implementacion, compartida kernel<->host.** El formato
  on-disk vive en `include/svfs/svfs_format.h` -- structs, checksum, bit-math,
  validacion y asignacion first-fit -- y lo compilan tanto el kernel como el
  tool de host. `libsvfs/` suma el core portable y `svfs-cli`, el tool nativo
  que hace **todas** las escrituras de `build/disk.img`; el escritor legacy en
  PowerShell, que hacia byte-poking del formato en paralelo, se borro. Esa
  doble implementacion era el origen historico de los bugs de desincronizacion.

### Eliminado

- **Retiradas las variantes en Haxe de About y Files (`aboutapp-hx`,
  `filesapp-hx`).** Eran demos de validacion de la cadena AOT, no apps
  oficiales: las de C siempre fueron las que se usan. Se van con sus harnesses
  headless, los targets `native-about`/`native-files` y las entradas del grupo
  **Native**, que desaparece del launcher. Con ellas se van los widgets de
  `haxe-toolkit/` que solo ellas usaban (`Listbox`, `Textview`,
  `Menubar`/`Menu`/`MenuItem`, `Dialog`); quedan `Painter` y `Boton`, que usa
  `sxguiapp`. Se conservan los runtimes `sx_sysinfo.c`/`sx_fs.c`: a diferencia
  del toolkit, que es bootstrap, son superficie declarada del ABI nativo.

- **Borrado `subsystems/posix/userland/busybox.c`.** El multicall hecho a mano
  ya no lo compilaba ningun build: los applets que se instalan salen de
  `vendor/busybox-port`. Se comparo applet por applet antes de sacarlo.

### Corregido

- **`realloc` podia colgarse al crecer un bloque.** El bucle de crecimiento
  in-place llamaba a `sx_merge_with_next` sobre un bloque **ocupado**, y ese
  helper vuelve sin hacer nada si el bloque no esta libre: cuando el siguiente
  era libre y adyacente, el bucle no avanzaba nunca. No se habia ejercitado
  porque el unico caso que lo tocaba (`sdk/posixsmoke`) no esta cableado a
  ningun build. Ahora `sx_absorb_next` avisa si absorbio algo y el bucle puede
  terminar.

- **El driver AC'97 truncaba a 32 bits la direccion del buffer de userland.**
  `copy_period` la recibia como `uint32_t`. Estuvo latente toda la vida del
  driver porque cualquier buffer de audio venia de la imagen ELF o de la BSS,
  por debajo de 4 GiB, donde truncar no hace nada; pero una direccion de
  userland puede estar mucho mas arriba -- una vista de seccion se mapea en
  `kSectionViewBase`, 64 GiB --, y ahi la direccion truncada queda basura,
  `copy_from_user` falla y `audio_write` devuelve EINVAL. Un cliente que apague
  su audio ante el primer error de `write` (Doom lo hace) se queda mudo el
  resto de la sesion. `virtio-sound` no tenia el bug: pasa el puntero entero.

  Nadie lo veia porque `audiotest` usaba un array de la BSS y ningun harness
  tocaba una direccion alta. Ahora pide su buffer de reproduccion con
  `section_create` + `map_view`, asi `smoke` y `ac97-count` ejercitan el camino
  de 64 bits.

- **Un smoke dejaba el spec de automatizacion pegado para siempre.** El build
  escribia `SMOKE` en el rootfs al correr un target automatizado pero nunca lo
  borraba, asi que se volvia a hornear en el initramfs en cada build posterior
  e `init` arrancaba ese runner en vez del escritorio -- en silencio y hasta el
  proximo `clean`. Ahora un build sin spec lo borra.

- **Al abrir una app se veia por un instante la ventana vacia del tamano
  generico.** El WM componia la ventana desde el fork, sin esperar el primer
  frame. Ahora no la compone hasta que el cliente pinta; mientras arranca, el
  feedback es el cursor WAIT.
- **Resize: la ventana quedaba medio negra.** `resize_overlay_client_surface`
  limpiaba la superficie **despues** de publicar las dimensiones nuevas en el
  header, borrandole al cliente el frame que acababa de copiar. Aparecio con el
  size hint, pero el bug estaba en el resize por bordes desde siempre.
- **El boton de power no apagaba la maquina: la SCI se ruteaba dos veces.**
  `acpi::start_sci()` y el glue de uACPI ruteaban la misma GSI, pisandose. Ahora
  una linea de interrupcion admite varios dueños: `register_interrupt_handler`
  encadena en vez de reemplazar y `ioapic::route_gsi` reconoce una GSI ya
  ruteada como compartida — lo que ademas cubre dos links PCI que resuelven a la
  misma GSI. Nueva `acpi::enable_power_button()`, que el bringup de uACPI vuelve
  a llamar porque `uacpi_initialize` apaga todos los eventos fijos.
- **VirtualBox con I/O APIC activado no terminaba de arrancar: soporte xAPIC
  MMIO en el APIC local.** `initialize_local_apic` solo sabia hablar x2APIC por
  MSR y VBox nunca lo expone, asi que el APIC local quedaba descartado; con el
  I/O APIC activo el firmware enmascara LINT0, el fallback del PIT moria y sin
  ticks el boot se congelaba en el splash. Ahora mapea la ventana MMIO que
  apunta `IA32_APIC_BASE` cuando no hay x2APIC, y repone LINT0 como ExtINT
  (virtual wire) para el camino PIC legacy. `ioapic::initialize` se niega a
  correr sin APIC local operativo. Nuevo log: `cpu: APIC local en modo
  xAPIC|x2APIC (id N)`.
- **Key Test** pisaba la segunda linea de ayuda con el primer evento, y **Mouse
  Test** cortaba la ultima linea de su panel.
- **El build no compilaba en Linux nativo.** `svfs-cli` (tool de host de
  `libsvfs`) no compilaba con `clang -std=c11` sobre glibc: `fseeko`/`off_t`
  quedan ocultos sin `_POSIX_C_SOURCE` en modo C estricto. Ademas
  `New-SvfsManifest` armaba las rutas relativas del manifiesto asumiendo
  separador `\` (`TrimStart('\')`), asi que en Linux (separador `/`) quedaba un
  `/` inicial y `svfs-cli apply` rechazaba el `mkdir` (`argumento/ruta
  invalida`). Ninguno de los dos afecta la rama Windows.
- **`exec`/`spawn` reportaban ENOENT para cualquier fallo de carga.** Lanzar un
  binario perfectamente instalado fallaba con `no such file or directory`
  cuando el problema real era falta de memoria, y mandaba a investigar el
  filesystem. Ahora cada paso reporta su motivo: `ENOMEM`, `ENOEXEC` para un
  ELF invalido, `EIO` si falla la lectura y `EACCES` si el path no es un
  archivo regular. El kernel ademas loguea el paso que fallo y las paginas
  libres, que es la unica traza de por que un binario que existe no arranca.
- **El WM se colgaba entero por un cliente que no drenaba su input.** Abrir una
  segunda instancia de un programa congelaba la sesion completa. El WM escribe
  el input a sus clientes en no-bloqueante, pero el kernel ignoraba
  `O_NONBLOCK` en las escrituras **parciales** a pipe: si entraba una parte y
  el resto no, bloqueaba igual.
- **El cursor se congelaba con el Task List abierto.** Solo saltaba a la
  posicion nueva al mover la seleccion con las flechas. El handler del Task
  List consumia el evento de puntero y se salteaba el repintado del cursor.
- **El `apply` sobre `build/disk.img` fallaba con "sin espacio contiguo" sin
  estar fragmentada.** Cualquier binario de userland de mas de 20400 bytes que
  cruzara un borde de sector al crecer reventaba el build, con espacio libre de
  sobra: el buffer donde `ensure_capacity` preserva el inodo estaba en el stack
  y tenia ese tope. Ademas la imagen se auto-compacta y el apply reintenta
  cuando de verdad no queda una corrida libre.
- **`ps` imprimia el literal `%-13s` en la columna STATE.** El `printf` del
  applet de busybox no soportaba el flag `-` de justificacion a la izquierda.

## [0.3.3] - 2026-07-09

### Agregado

- **Audio en VirtualBox: driver AC'97 + HAL de audio con backends.** El audio
  solo funcionaba con `virtio-sound-pci` (QEMU); VirtualBox no emula ese chip y
  todo quedaba mudo, Doom incluido. El subsistema pasa a un HAL espejo del de
  display: dispatcher `audio::` con vtable `Backend`, un `audio_device.cpp`
  agnostico que registra `/dev/audio0` y concentra la logica comun, y dos
  backends — `virtio_sound` y el nuevo `ac97` (`kernel/ac97.cpp`), que maneja el
  controlador Intel ICH por bus-master DMA con **polling puro de CIV**, sin IRQ,
  esquivando el INTx legacy que no llega en VBox. El formato fijo del ABI (S16
  estereo 48 kHz) es el rate nativo del DAC, asi que no hay resampling.
  `savanxp_audio_backend` suma `SAVANXP_AUDIO_BACKEND_AC97`. Confirmado con
  sonido en VirtualBox real y en QEMU.
- **LiveCD: `/disk` autocontenido en la ISO via ramdisk escribible.** `disk.img`
  viaja dentro de la ISO como un segundo modulo de Limine y el kernel la expone
  como block device en memoria, asi que la ISO arranca sola con `/disk` montado
  — antes las apps de `/disk` (Doom, etc.) no aparecian al bootear sin el disco
  de dev. Es escribible-efimero in-place: los cambios se pierden al reiniciar,
  la semantica correcta de un LiveCD. Un disco IDE persistente mantiene
  prioridad. La instalacion persistente a disco queda como fase futura.
- **Doom con Freedoom (IWAD libre) en el LiveCD.** `sdk/doomgeneric/build.ps1`
  hornea `freedoom1.wad` por defecto en `/disk/games/doom/`, para que la ISO
  distribuible lleve Doom jugable sin contenido propietario. El motor igual
  detecta `doom1.wad`/`doom.wad` si el usuario los aporta.
- **Capa IOAPIC/MADT y ruteo de IRQs por GSI.** Parsea la MADT (IOAPICs e
  Interrupt Source Overrides), programa las redirection entries y rutea GSIs a
  vectores de la IDT con entrega por el Local APIC (`ioapic::route_gsi` /
  `route_legacy_irq`), con un pool de vectores 50-63 reservado en `cpu_init`.
  PS/2 migra a este ruteo cuando hay IOAPIC, con fallback al PIC legacy si no
  hay MADT. Es el prerrequisito para la SCI de ACPI y el INTx resuelto por `_PRT`.
- **ACPI: SCI ruteada por IOAPIC + boton de power.** `acpi::start_sci()` habilita
  el modo ACPI, enmascara todas las GPE (sin interprete AML, para evitar
  tormentas en la SCI level-triggered), habilita el evento fijo PWRBTN y rutea la
  SCI, con fallback al PIC. El handler dispara `acpi::shutdown()` (S5, el mismo
  camino que `/dev/power`). El apagado es inmediato, sin cierre graceful de
  userland.
- **uACPI vendorizado e integrado: interprete de AML real.** Copia horneada de
  uACPI v6.0.0 bajo `vendor/uacpi/`, sin submodulo (misma convencion que
  busybox), para reemplazar progresivamente la ACPI hand-rolled. Se compila como
  C11 con su propia variable de flags en Ninja. Glue en `kernel/uacpi_glue.cpp`:
  capa `uacpi_kernel_*` sobre heap/vmm/pci/timer/ioapic, con el tiempo tomado del
  TSC calibrado contra el PIT porque en el bringup las interrupciones estan
  apagadas. `bringup()` corre `uacpi_initialize` + `uacpi_namespace_load`
  conviviendo con la ACPI hand-rolled; `uacpi_namespace_initialize()` queda
  diferida a la etapa de eventos.
- **Ruteo de INTx via `_PRT` de uACPI, cerrado de punta a punta.**
  `route_pci_intx(bus, dev, func, handler)` lee el Interrupt Pin de la config
  PCI, busca la entrada `_PRT` del root bridge, resuelve el link device a su GSI
  real evaluando el `_CRS` y programa el IOAPIC con la polaridad y el trigger que
  declara el firmware. El `rtl8139` pasa de polling a interrupt-driven sobre este
  camino, con fallback a polling si el ruteo falla. INTx legacy ahora llega en
  q35+APIC, que era el bottleneck que habia forzado usar MSI-X para virtio-gpu.
- **Subsistema nativo — Fase 2: ABI nativo v1 + runtime real.** El contrato
  kernel<->userland vive en `savanxp_native_abi.h` como unica fuente: espacio de
  syscalls particionado (`< 0x1000` baseline delegado en posix, `>= 0x1000`
  propias, que para un proceso posix no existen), version de ABI con handshake
  obligatorio al arrancar (aborta con exit 132 si no coincide) y las dos primeras
  syscalls nativas (`SXN_SYS_INFO`, `SXN_SYS_LOG`). El runtime suma heap propio,
  builtins de memoria, `operator new/delete` y un mini `<memory>` freestanding,
  con lo cual las clases Haxe ya compilan y corren sin libstdc++.
- **Subsistema nativo — override `_std`: String y Array reales de Haxe.** El
  `_std` de reflaxe.CPP se expone como overrides `*.cross.hx` generados por el
  build, que es el mecanismo oficial de Haxe para overrides por plataforma y no
  envenena el contexto macro. El mini std C++ freestanding crece con `<string>`,
  `<deque>`, `<initializer_list>`, `<algorithm>`, `<cctype>` y `<new>`. Dos fixes
  al codegen sin parchear las libs pineadas: un shadow de `Math.hx` y el
  preprocesador `UniqueLocalNames`, porque el codegen aplana bloques hermanos y
  los contadores de dos for-in colisionaban en el mismo scope de C++.
- **Subsistema nativo — ABI gfx + hello GUI en Haxe.** Bloque de syscalls de
  graficos (`0x1010`: GFX_INFO / ACQUIRE / RELEASE / PRESENT). El display es
  parte del ABI de primera clase, sin `/dev/gpu0` ni ioctls, compartiendo los
  internals de `display::` y la sesion exclusiva por pid con los ioctls de posix.
  Documentado: Float de Haxe todavia no funciona en freestanding.
- **Subsistema nativo — protocolo cliente del compositor (apps ventaneadas).**
  Capa `sxn_gui_*` que habla el contrato de superficie v3 sobre los fds 3..9,
  todo sobre syscalls del baseline y sin kernel nuevo. Primera app ventaneada
  nativa: `nativegui`. Verificacion headless con `test/guihost.c`, que de paso es
  el primer test del cambio de subsistema via exec (fork posix -> exec ELF
  nativo).
- **Header esperable generico en el Object Manager (`object::Header`).** El
  estado de senializacion que vivia duplicado por tipo sube a la base comun
  (`waitable`/`manual_reset`/`signal_count`), asi que un tipo esperable nuevo ya
  no obliga a tocar el despachador de esperas.
- **Semaforo real (`SAVANXP_SYS_SEMAPHORE_CREATE`/`_RELEASE`).** Primer uso del
  header generico: `create_semaphore(initial, max)` y `release_semaphore`
  (satura en `max_count` y rechaza sin tocar nada el release que lo excederia),
  con wrappers en el SDK y `semaphoretest` enganchado en la suite `smoke`.
- **`build.ps1 run`/`debug`: soporte `-Accel whpx`.** Acelera QEMU con Windows
  Hypervisor Platform; por defecto se sigue usando TCG. Fuerza `-cpu qemu64`
  porque `-cpu max`/`host` bajo whpx hacen crashear a OVMF con un `#GP` en
  `PlatformPei`. Los targets automatizados quedan hardcodeados en TCG a
  proposito, para no meterle no-determinismo a los tests.
- **`virtio-sound`: fin del enmudecimiento silencioso.** Si el dispositivo
  responde pero ningun stream de salida ofrece el formato fijo del ABI, el fallo
  ahora se registra por consola en vez de no registrar `/dev/audio0` sin dejar
  traza.

### Cambiado

- **Compilacion de kernel+userland via Ninja.** Reemplaza la fase secuencial y
  sin incremental de `build.ps1` (~250-300 fuentes, archivo por archivo) por
  Ninja: paralelo y con tracking de dependencias de headers via `-MMD`, pineado
  en el toolchain igual que LLVM/QEMU/xorriso. El link, la imagen SVFS2, la ISO
  y QEMU siguen manejados por `build.ps1` sin cambios.
- `savanxp_mode_bits` (SDK) pierde el tipo subyacente fijo del enum, que
  disparaba `-Wfixed-enum-extension` en cada TU que incluye `syscall.h`. Sin
  cambio funcional.

### Corregido

- **`virtio-gpu`: `SET_SCANOUT` colgado para siempre bajo WHPX.** El boot se
  trababa en "Preparando display". El segundo tier de espera caia a `HLT`, y
  durante el boot temprano el kernel corre con `IF=0`: un `HLT` con
  interrupciones deshabilitadas solo despierta con NMI, nunca con la IRQ del
  timer. TCG es mas laxo en ese caso puntual, por eso el bug nunca se habia
  manifestado. Ahora el wait usa `timer::monotonic_ns()` (reloj por TSC, que
  avanza sin importar el estado de interrupciones) con busy-spin acotado.
- **virtio-sound: reproduccion TX async multi-buffer.** El camino TX enviaba un
  periodo y hacia spin esperando que el device lo consumiera — con interrupciones
  deshabilitadas, el mismo riesgo de congelar el reloj del guest que se corrigio
  en AC'97. Ahora la cola usa un anillo de `kTxSlots` periodos y `submit_period`
  encola sin esperar; si el anillo esta lleno descarta en vez de bloquear.
- **AC'97: reproduccion sin bloqueo, colchon de silencio y sin IOC.** Tres
  correcciones al audio entrecortado en VirtualBox: `submit_period` deja de
  hacer spin con IRQ off (congelaba el timer y con el el reloj del guest, que es
  el que Doom usa para dosificar el audio *y* para su logica); las entradas del
  BDL pierden el bit interrupt-on-completion, que disparaba una interrupcion por
  periodo que nadie atendia; y al preparar el stream se precargan periodos de
  silencio, reinyectados si el ring se vacia, para absorber el jitter del
  productor. Nuevos targets `ac97-count` (contador de underruns del driver) y
  `ac97-stream` (captura a WAV, util solo con aceleracion por hardware).
- **Audio "robotizado"/trabado en Doom (subalimentacion del device).**
  `DG_Sound_Update` mezclaba N frames avanzando la posicion de todos los canales
  pero escribia solo ese total redondeado *hacia abajo* al periodo: a ~35 Hz eso
  alimentaba el device a ~0.75x del ritmo de reproduccion y ademas adelantaba los
  efectos ~1.34x. Ahora escribe todos los frames mezclados. Estaba en la capa
  comun del glue, por eso sonaba igual en virtio y en AC'97.
- **Residuos visuales del cursor sobre elementos del compositor.**
  `sx_painter_draw_frame` dibujaba el marco del rect ya intersectado con el clip,
  asi que al repintar un elemento en fragmentos cada fragmento recibia su propio
  borde y quedaban lineas dentro del elemento. Ahora traza el marco del rect
  original como cuatro tiras recortadas por `fill_rect`. Nuevo test de regresion
  headless `build.ps1 cursor-repro`.
- **Pulido de `fb_gpu` y `virtio-gpu`.** `present_region` de `fb_gpu` leia el
  origen desde la fila 0 en vez del offset (x,y) de la superficie, pintando mal
  los presents parciales en VirtualBox, y `GET_STATS` devolvia un struct en cero;
  `refresh_scanouts` de virtio-gpu ya no pisa un flip fullscreen al llegar un
  evento de display; el header `used` de la cola de cursor se lee volatile;
  `notify_off_multiplier == 0` deja de hacer que se saltee el notify; y
  `REFRESH_SCANOUTS` exige la sesion grafica como el resto de los ioctls que
  mutan estado. El soak de `gputest` suma un subtest de `SET_MODE` en runtime.
- **`build.ps1` bifurcaba silenciosamente a PowerShell 5.1 para generar assets.**
  `& powershell` resuelve siempre a Windows PowerShell sin importar el motor que
  arranco el build, asi que correr todo con `pwsh` 7 daba una falsa sensacion de
  portabilidad: el tramo que usa GDI+ nunca se ejecutaba bajo pwsh. Ahora los
  scripts se invocan in-process.
- **Rutas de `Join-Path` con `\` embebido, incompatibles fuera de Windows.** En
  Windows funcionaba por casualidad (.NET colapsa `\\`, `\` y `/`); en Linux `\`
  queda como caracter literal del nombre. ~18 ocurrencias normalizadas a `/`.
  Fuera de alcance a proposito: `ConvertTo-CygwinPath` y el contenido de
  `startup.nsh`.

## [0.3.2] - 2026-07-03

### Agregado

- **sxgui completo: toolkit de widgets estilo Win9x.** La libreria del SDK pasa
  de 5 controles basicos a un toolkit completo, manteniendo el modelo
  retained-mode allocation-free (la app posee el array plano de widgets, los
  buffers de texto y las tablas de items; el toolkit solo pinta y despacha
  input): recorrido de foco con Tab/Shift+Tab, textfield con caret real,
  scrollbar como widget y listbox con scroll, double-click con motivo de accion
  (`CLICK`/`CHANGE`/`ACTIVATE`), radio buttons por group id, combobox con
  dropdown dentro del backbuffer propio (sin ventanas hijas), barra de menu con
  desplegables y `on_command(id)`, dialogos modales (sin loop anidado: una
  maquina de estados dentro del mismo main loop), groupbox, progress bar y
  textview multilinea.
- **App frame `sxgui_app`.** Encapsula la sesion gfx y el main loop que toda app
  de widgets repetia (poll de teclado/puntero, RESIZED, repaint gateado, present
  y throttle de 16 ms), con hooks opcionales `on_key`/`on_paint`/`on_resize`.
  ESC cierra la app salvo que el toolkit lo consuma antes.
- `widgetsdemo` crece como galeria de referencia de todo el toolkit.

### Cambiado

- **`aboutapp` y `filesapp` portadas a sxgui.** aboutapp queda declarativa;
  filesapp conserva toda su logica de filesystem pero delega en el toolkit la
  lista con scroll, el preview, la barra de menu y el statusbar. Ambas pierden
  su backbuffer estatico de 8 MiB y el loop de eventos manual.
- **La arena de malloc del SDK baja de 48 MiB a 8 MiB por defecto.** El heap
  estatico vive en la BSS y el kernel mapea la BSS entera al exec, asi que cada
  app del sistema costaba ~50 MiB residentes y con tres abiertas se agotaba la
  memoria fisica. El build externo del SDK conserva los 48 MiB via
  `-DSX_HEAP_SIZE` para apps pesadas como Doom.

### Corregido

- **Fuga de memoria fisica en el fork por paginas de section views.**
  `vm::clone_address_space` alocaba y copiaba una pagina por cada pagina de
  usuario presente y recien despues descartaba las de section views, sin liberar
  la copia. Como el desktop mapea las vistas de todas las superficies cliente,
  cada launch perdia ~4 MiB por vista y tras unos pocos ningun `exec` volvia a
  funcionar hasta reiniciar.
- `desktop_client.path` guardaba el puntero recibido al lanzar, que para los
  launches pedidos por clientes apuntaba al buffer de stack del request: el
  titulo de ventana y los logs leian memoria colgante. Ahora el cliente guarda
  una copia propia.

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
- **Corrupcion de inodos por redondeo en el bitmap de SVFS2.**
  `Get-Svfs2BitmapBit`/`Set-Svfs2BitmapBit` (instalador host-side en
  `tools/UserAppCommon.ps1`) calculaban el indice de byte con `[int]($Bit / 8)`,
  que en PowerShell redondea en vez de truncar: el bit caia en el byte
  equivocado y el bitmap del host quedaba desincronizado con el indexado floor
  del kernel, que reasignaba inodos vivos (sintoma: "inode esperado N pero se
  leyo 0" tras un ciclo de boot). Ahora usan division entera (`-shr 3`).
- Apagado ordenado de QEMU en `Run-AutomationQemu` (smoke/selftest/soak): en vez
  de `Stop-Process -Force`, se pide `quit` por el monitor HMP para que QEMU
  vacie sus backends de bloque y cierre el archivo de disco limpiamente, con
  fallback al kill forzado si el monitor no responde.
- El SO quedaba colgado indefinidamente justo despues de "Iniciando bienvenida"
  en hipervisores sin x2APIC (confirmado en VirtualBox con backend `VBoxVGA`):
  `initialize_local_apic` fallaba en silencio y el scheduler nunca arrancaba, a
  pesar de que el kernel seguia vivo. Resuelto por el fallback a `PIT` agregado
  arriba.
- **Cursor erratico: framing de paquetes PS/2 corrompido en streaming.**
  `process_mouse_byte` descartaba todo byte `0xFA`/`0xFE` asumiendo que eran
  ACK/RESEND, pero en streaming esos valores tambien son deltas legitimos (`-6`
  y `-2`): perder ese byte desincronizaba el framing de 3 bytes. El bug era
  direccional — solo hacia izquierda/abajo, porque los deltas positivos nunca
  coinciden con esos valores. Los ACK/RESEND reales se consumen de forma
  sincronica en la inicializacion y no pasan por este camino. Se suma un clamp
  defensivo de +-150 por eje, que recorta en vez de descartar el paquete.
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
  fixes de esta version el sistema arranca estable hasta sesion grafica y el
  mouse PS/2 responde en las cuatro direcciones. Pendiente y sin diagnosticar:
  los binarios de `/disk` (`doomgeneric`, `smoke`, `gputest`) todavia no
  arrancan bajo VirtualBox aun adjuntando el volumen SVFS2.

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
