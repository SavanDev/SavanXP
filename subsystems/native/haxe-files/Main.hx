// SavanXP - filesapp portada a Haxe (Fase 3).
//
// Port (v1: navegador de directorios) de subsystems/posix/userland/filesapp.c.
// Lista el contenido de un directorio en un Listbox (con ".." para subir),
// permite navegar (click/Enter en un directorio entra; ".." o Backspace sube) y
// muestra una barra de estado. La lectura del filesystem va por sx_fs.c (open/
// readdir/stat del baseline). Preview de archivos, menubar y dialog quedan para
// follow-ups. Usa el toolkit compartido (Painter, Boton, Listbox).

class Explorador {
  var p:Painter;
  var lista:Listbox;
  public var rutaActual:String;
  var estado:String;
  public var salir:Bool;

  public function new(p:Painter) {
    this.p = p;
    this.lista = new Listbox(12, 30, p.ancho - 24, p.alto - 70);
    this.rutaActual = "/";
    this.estado = "";
    this.salir = false;
    cargar("/");
  }

  inline function istr(n:Int):String {
    return untyped __cpp__("std::to_string({0})", n);
  }

  public function cargar(ruta:String) {
    var n:Int = untyped __cpp__("sxn_fs_load({0}.c_str())", ruta);
    lista.items = [];
    if (n < 0) {
      estado = "No se pudo abrir el directorio";
      return;
    }
    rutaActual = ruta;
    var i = 0;
    while (i < n) {
      var esdir:Bool = untyped __cpp__("sxn_fs_is_dir({0}) != 0", i);
      var nombre:String = untyped __cpp__("std::string(sxn_fs_name({0}))", i);
      var etiqueta = (esdir ? "[DIR] " : "[ARCH] ") + nombre;
      lista.items.push(etiqueta);
      i += 1;
    }
    lista.seleccion = 0;
    lista.scroll = 0;
    estado = istr(n) + " elemento(s)";
    // Log para el harness/serial: confirma la navegacion.
    untyped __cpp__('sxn_log({0}.c_str())', rutaActual);
    untyped __cpp__('sxn_log_num("files: entradas", {0})', n);
  }

  function subir() {
    var padre:String = untyped __cpp__("std::string(sxn_fs_parent({0}.c_str()))", rutaActual);
    cargar(padre);
  }

  function activar(idx:Int) {
    if (idx < 0 || idx >= lista.items.length) {
      return;
    }
    var nombre:String = untyped __cpp__("std::string(sxn_fs_name({0}))", idx);
    var esdir:Bool = untyped __cpp__("sxn_fs_is_dir({0}) != 0", idx);
    if (nombre == "..") {
      subir();
      return;
    }
    if (esdir) {
      var nuevo:String = untyped __cpp__(
        "std::string(sxn_fs_join({0}.c_str(), {1}.c_str()))", rutaActual, nombre);
      cargar(nuevo);
    } else {
      estado = "Archivo: " + nombre;
    }
  }

  public function repintar() {
    p.rect(0, 0, p.ancho, p.alto, Painter.FACE);
    p.label(12, 8, "Ruta: " + rutaActual);
    lista.dibujar(p);
    // Barra de estado hundida.
    var sy = p.alto - 30;
    p.rect(12, sy, p.ancho - 24, 20, Painter.FACE);
    p.sunken(12, sy, p.ancho - 24, 20);
    p.label(16, sy + 2, estado);
  }

  public function puntero(mx:Int, my:Int, izq:Bool, izqPrev:Bool):Bool {
    // Click (transicion a soltar) selecciona; si ya estaba seleccionado, activa.
    if (!izq && izqPrev) {
      var idx = lista.indiceEn(mx, my, p);
      if (idx >= 0) {
        if (idx == lista.seleccion) {
          activar(idx);
        } else {
          lista.seleccion = idx;
        }
        return true;
      }
    }
    return false;
  }

  public function tecla(key:Int):Bool {
    if (key == 27) { // ESC
      salir = true;
      return false;
    }
    if (key == 256) { // arriba
      if (lista.seleccion > 0) {
        lista.seleccion -= 1;
        lista.asegurarVisible(p);
        return true;
      }
      return false;
    }
    if (key == 257) { // abajo
      if (lista.seleccion < lista.items.length - 1) {
        lista.seleccion += 1;
        lista.asegurarVisible(p);
        return true;
      }
      return false;
    }
    if (key == 13) { // Enter: activar
      activar(lista.seleccion);
      return true;
    }
    if (key == 8) { // Backspace: subir
      subir();
      return true;
    }
    if (key == 274) { // F5: refrescar
      cargar(rutaActual);
      return true;
    }
    return false;
  }
}

function main() {
  var abierto:Int = untyped __cpp__("(int)sxn_gui_open()");
  if (abierto != 0) {
    untyped __cpp__('sxn_log_num("files: sin sesion de ventana", {0})', abierto);
    untyped __cpp__("sxn_exit(2)");
  }

  var ancho:Int = untyped __cpp__("(int)sxn_gui_width()");
  var alto:Int = untyped __cpp__("(int)sxn_gui_height()");
  var stride:Int = untyped __cpp__("(int)sxn_gui_stride_pixels()");

  var p = new Painter(ancho, alto, stride);
  var exp = new Explorador(p);
  exp.repintar();
  if (p.present() != 0) {
    untyped __cpp__('sxn_log("files: fallo el primer present")');
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
      if (exp.puntero(mx, my, izq, izqPrev)) {
        cambio = true;
      }
      prevBtn = btn;
    }

    untyped __cpp__("struct sxn_gui_input_event __ev; int __er = sxn_gui_poll_event(&__ev)");
    if (untyped __cpp__("__er == 1 && __ev.type == SXN_GUI_EVENT_KEY_DOWN")) {
      var key:Int = untyped __cpp__("(int)__ev.key");
      if (exp.tecla(key)) {
        cambio = true;
      }
    }

    if (cambio) {
      exp.repintar();
      if (p.present() != 0) {
        untyped __cpp__('sxn_log("files: fallo present en el loop")');
        untyped __cpp__("sxn_exit(1)");
      }
    }

    if (exp.salir) {
      break;
    }

    untyped __cpp__("sxn_sleep_ms(8)");
  }

  untyped __cpp__('sxn_log("files: filesapp (Haxe) OK")');
  untyped __cpp__("sxn_gui_close()");
}
