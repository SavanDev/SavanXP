// SavanXP - aboutapp portada a Haxe (Fase 3).
//
// Port de subsystems/posix/userland/aboutapp.c: ventana "Acerca de" con labels,
// group boxes e info del sistema (version/uptime/procesos/memoria/disco/reloj),
// mas botones Refrescar/Cerrar y F5 para refrescar. Usa el toolkit compartido
// (haxe-toolkit: Painter, Boton) y los accessors de sysinfo del runtime nativo
// (sx_sysinfo.c). Cliente del compositor; el escritorio pone la barra de titulo.

class About {
  var p:Painter;
  var refrescar:Boton;
  var cerrar:Boton;
  var capturado:Boton;
  public var salir:Bool;

  // Lineas de info del sistema (grupo "Sistema").
  var lVersion:String;
  var lUptime:String;
  var lProc:String;
  var lMem:String;
  var lDisk:String;
  var lClock:String;

  public function new(p:Painter) {
    this.p = p;
    this.refrescar = new Boton(16, 392, 100, 26, "Refrescar");
    this.cerrar = new Boton(128, 392, 100, 26, "Cerrar");
    this.capturado = null;
    this.salir = false;
    actualizar();
  }

  inline function istr(n:Int):String {
    return untyped __cpp__("std::to_string({0})", n);
  }

  function dos(n:Int):String {
    return n < 10 ? "0" + istr(n) : istr(n);
  }

  // Re-lee la info del sistema y rearma las lineas (equivalente a refresh_info).
  public function actualizar() {
    untyped __cpp__("sxn_sys_refresh()");
    var version:String = untyped __cpp__("std::string(sxn_sys_version())");
    var up:Int = untyped __cpp__("sxn_sys_uptime_ms()");
    var procs:Int = untyped __cpp__("sxn_sys_process_count()");
    var memU:Int = untyped __cpp__("sxn_sys_memory_usable_mib()");
    var memR:Int = untyped __cpp__("sxn_sys_memory_reclaimable_mib()");
    var dU:Int = untyped __cpp__("sxn_sys_disk_used_mib()");
    var dT:Int = untyped __cpp__("sxn_sys_disk_total_mib()");

    lVersion = "Version: " + version;
    lUptime = "Uptime: " + istr(up) + " ms";
    lProc = "Procesos: " + istr(procs) + " activos";
    lMem = "Memoria: " + istr(memU) + " MiB usable, " + istr(memR) + " MiB reclaimable";
    lDisk = "Disco: " + istr(dU) + " / " + istr(dT) + " MiB usado";

    if (untyped __cpp__("sxn_sys_clock_valid()")) {
      var hh:Int = untyped __cpp__("sxn_sys_clock_hour()");
      var mm:Int = untyped __cpp__("sxn_sys_clock_minute()");
      var ss:Int = untyped __cpp__("sxn_sys_clock_second()");
      lClock = "Reloj: " + dos(hh) + ":" + dos(mm) + ":" + dos(ss);
    } else {
      lClock = "Reloj: no disponible";
    }
  }

  public function repintar() {
    p.rect(0, 0, p.ancho, p.alto, Painter.FACE);

    p.label(16, 10, "Acerca de SavanXP");
    p.label(16, 30, "SO de escritorio experimental, compositor-first");

    p.groupbox(16, 56, 424, 148, "Sistema");
    p.label(28, 76, lVersion);
    p.label(28, 96, lUptime);
    p.label(28, 116, lProc);
    p.label(28, 136, lMem);
    p.label(28, 156, lDisk);
    p.label(28, 176, lClock);

    p.groupbox(16, 214, 424, 88, "Escritorio");
    p.label(28, 234, "El menu Inicio abre apps en ventanas overlay");
    p.label(28, 254, "La barra de tareas restaura o minimiza ventanas");
    p.label(28, 274, "Controles de ventana: minimizar, maximizar, cerrar");

    p.groupbox(16, 312, 424, 68, "Teclado");
    p.label(28, 332, "SUPER abre Inicio   ESC cierra esta ventana");
    p.label(28, 352, "F5 o el boton Refrescar actualiza los valores");

    refrescar.dibujar(p);
    cerrar.dibujar(p);
  }

  function accionar(b:Boton) {
    if (b == refrescar) {
      actualizar();
    } else {
      salir = true;
    }
  }

  public function puntero(mx:Int, my:Int, izq:Bool, izqPrev:Bool):Bool {
    if (izq && !izqPrev) {
      if (refrescar.contiene(mx, my)) {
        refrescar.pressed = true;
        capturado = refrescar;
        return true;
      } else if (cerrar.contiene(mx, my)) {
        cerrar.pressed = true;
        capturado = cerrar;
        return true;
      }
    } else if (!izq && izqPrev) {
      if (capturado != null) {
        var b = capturado;
        b.pressed = false;
        capturado = null;
        if (b.contiene(mx, my)) {
          accionar(b);
        }
        return true;
      }
    }
    return false;
  }

  // Devuelve true si hay que repintar. F5 refresca, ESC cierra.
  public function tecla(key:Int):Bool {
    if (key == 27) {
      salir = true;
      return false;
    }
    if (key == 274) { // F5
      actualizar();
      return true;
    }
    return false;
  }
}

function main() {
  var abierto:Int = untyped __cpp__("(int)sxn_gui_open()");
  if (abierto != 0) {
    untyped __cpp__('sxn_log_num("about: sin sesion de ventana", {0})', abierto);
    untyped __cpp__("sxn_exit(2)");
  }

  // La ventana la pide la app: su layout es fijo (456x434) y el WM, que no lo
  // conoce, le daria la superficie generica del escritorio.
  if (untyped __cpp__("(int)sxn_gui_request_content_size(456u, 434u)") == 0) {
    untyped __cpp__("sxn_gui_wait_content_size(250)");
  }

  var ancho:Int = untyped __cpp__("(int)sxn_gui_width()");
  var alto:Int = untyped __cpp__("(int)sxn_gui_height()");
  var stride:Int = untyped __cpp__("(int)sxn_gui_stride_pixels()");

  var p = new Painter(ancho, alto, stride);
  var about = new About(p);

  // Log de un par de valores para confirmar que las syscalls de info andan.
  untyped __cpp__('sxn_log_num("about: procesos activos", {0})', untyped __cpp__("sxn_sys_process_count()"));
  untyped __cpp__('sxn_log_num("about: memoria usable MiB", {0})', untyped __cpp__("sxn_sys_memory_usable_mib()"));

  about.repintar();
  if (p.present() != 0) {
    untyped __cpp__('sxn_log("about: fallo el primer present")');
    untyped __cpp__("sxn_exit(1)");
  }

  var prevBtn = 0;
  while (true) {
    if (untyped __cpp__("sxn_gui_should_close()")) {
      break;
    }

    var cambio = false;

    untyped __cpp__("struct sxn_gui_pointer_event __pt; int __pr");
    while (true) {
      untyped __cpp__("__pr = sxn_gui_poll_pointer(&__pt)");
      if (untyped __cpp__("__pr != 1")) {
        break;
      }
      var mx:Int = untyped __cpp__("(int)__pt.x");
      var my:Int = untyped __cpp__("(int)__pt.y");
      var btn:Int = untyped __cpp__("(int)__pt.buttons");
      var izq = (btn & 1) != 0;
      var izqPrev = (prevBtn & 1) != 0;
      if (about.puntero(mx, my, izq, izqPrev)) {
        cambio = true;
      }
      prevBtn = btn;
    }

    untyped __cpp__("struct sxn_gui_input_event __ev; int __er = sxn_gui_poll_event(&__ev)");
    if (untyped __cpp__("__er == 1 && __ev.type == SXN_GUI_EVENT_KEY_DOWN")) {
      var key:Int = untyped __cpp__("(int)__ev.key");
      if (about.tecla(key)) {
        cambio = true;
      }
    }

    if (cambio) {
      about.repintar();
      if (p.present() != 0) {
        untyped __cpp__('sxn_log("about: fallo present en el loop")');
        untyped __cpp__("sxn_exit(1)");
      }
    }

    if (about.salir) {
      break;
    }

    untyped __cpp__("sxn_sleep_ms(8)");
  }

  untyped __cpp__('sxn_log("about: aboutapp (Haxe) OK")');
  untyped __cpp__("sxn_gui_close()");
}
