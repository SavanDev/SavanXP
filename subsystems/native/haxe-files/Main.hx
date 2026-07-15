// SavanXP - filesapp portada a Haxe (Fase 3).
//
// Port de subsystems/posix/userland/filesapp.c: navegador de directorios con
// panel de preview. Lista el contenido de un directorio en un Listbox (con
// ".." para subir) y muestra en un Textview la ruta / tipo / tamano y las
// primeras lineas del archivo seleccionado. Navegacion: click/Enter entra a un
// directorio, ".."/Backspace sube, flechas mueven la seleccion, F5 refresca,
// ESC cierra. El filesystem se lee por sx_fs.c (open/readdir/stat/read del
// baseline). Menubar y dialog About quedan como follow-ups.
//
// Usa el toolkit compartido (Painter, Listbox, Textview).

class Explorador {
  var p:Painter;
  var lista:Listbox;
  var preview:Textview;
  public var rutaActual:String;
  var estado:String;
  public var salir:Bool;

  var previewX:Int;
  var panelY:Int;

  public function new(p:Painter) {
    this.p = p;
    this.panelY = 50;
    var listW = Std.int(p.ancho / 2) - 18;
    var panelH = p.alto - panelY - 40;
    this.previewX = 12 + listW + 12;
    var previewW = p.ancho - previewX - 12;

    this.lista = new Listbox(12, panelY, listW, panelH);
    this.preview = new Textview(previewX, panelY, previewW, panelH);
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
    untyped __cpp__('sxn_log({0}.c_str())', rutaActual);
    untyped __cpp__('sxn_log_num("files: entradas", {0})', n);
    actualizarPreview();
  }

  // Reconstruye el panel de preview segun la entrada seleccionada (equivalente
  // a filesapp_update_preview).
  function actualizarPreview() {
    preview.lines = [];
    var total = lista.items.length;
    var sel = lista.seleccion;
    if (total <= 0 || sel < 0 || sel >= total) {
      preview.lines.push("Sin entradas.");
      return;
    }

    var nombre:String = untyped __cpp__("std::string(sxn_fs_name({0}))", sel);
    if (nombre == "..") {
      preview.lines.push("Ir al directorio padre");
      return;
    }

    var full:String = untyped __cpp__(
      "std::string(sxn_fs_join({0}.c_str(), {1}.c_str()))", rutaActual, nombre);
    var traza = "preview: " + full;
    untyped __cpp__('sxn_log({0}.c_str())', traza);

    preview.lines.push(full);
    preview.lines.push("");

    if (untyped __cpp__("sxn_fs_path_is_dir({0}.c_str()) != 0", full)) {
      preview.lines.push("Directorio");
      preview.lines.push("Abrir: Enter o segundo click");
      return;
    }

    var tam:Int = untyped __cpp__("sxn_fs_size({0}.c_str())", full);
    preview.lines.push("Tamano: " + istr(tam) + " bytes");
    preview.lines.push("");

    var lineas:Int = untyped __cpp__("sxn_fs_preview_load({0}.c_str())", full);
    untyped __cpp__('sxn_log_num("files: lineas de preview", {0})', lineas);
    if (lineas <= 0) {
      preview.lines.push("(vacio o sin preview)");
      return;
    }
    var i = 0;
    while (i < lineas) {
      var linea:String = untyped __cpp__("std::string(sxn_fs_preview_line({0}))", i);
      preview.lines.push(linea);
      i += 1;
    }
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
      actualizarPreview();
      estado = "Preview actualizado";
    }
  }

  function seleccionar(idx:Int) {
    lista.seleccion = idx;
    lista.asegurarVisible(p);
    actualizarPreview();
  }

  public function repintar() {
    p.rect(0, 0, p.ancho, p.alto, Painter.FACE);
    p.label(12, 8, "Ruta: " + rutaActual);
    p.label(12, 30, "Directorio");
    p.label(previewX, 30, "Preview");

    lista.dibujar(p);
    preview.dibujar(p);

    var sy = p.alto - 30;
    p.rect(12, sy, p.ancho - 24, 20, Painter.FACE);
    p.sunken(12, sy, p.ancho - 24, 20);
    p.label(16, sy + 2, estado);
  }

  public function puntero(mx:Int, my:Int, izq:Bool, izqPrev:Bool):Bool {
    // Click (al soltar) selecciona; si ya estaba seleccionado, activa.
    if (!izq && izqPrev) {
      var idx = lista.indiceEn(mx, my, p);
      if (idx >= 0) {
        if (idx == lista.seleccion) {
          activar(idx);
        } else {
          seleccionar(idx);
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
        seleccionar(lista.seleccion - 1);
        return true;
      }
      return false;
    }
    if (key == 257) { // abajo
      if (lista.seleccion < lista.items.length - 1) {
        seleccionar(lista.seleccion + 1);
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
