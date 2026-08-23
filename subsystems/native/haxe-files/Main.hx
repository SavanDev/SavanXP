// SavanXP - filesapp portada a Haxe (Fase 3).
//
// Port de subsystems/posix/userland/filesapp.c: navegador de directorios con
// panel de preview. Lista el contenido de un directorio en un Listbox (con
// ".." para subir) y muestra en un Textview la ruta / tipo / tamano y las
// primeras lineas del archivo seleccionado. Navegacion: click/Enter entra a un
// directorio, ".."/Backspace sube, flechas mueven la seleccion, F5 refresca,
// ESC cierra. Tiene barra de menu (Archivo/Ayuda), dialog modal "Acerca de" y
// lanza ejecutables de /bin y /disk/bin pidiendoselo al escritorio (fd 9). El
// filesystem se lee por sx_fs.c (open/readdir/stat/read del baseline).
//
// Usa el toolkit compartido (Painter, Listbox, Textview, Menubar, Dialog).

class Explorador {
  var p:Painter;
  var menubar:Menubar;
  var lista:Listbox;
  var preview:Textview;
  var dialog:Dialog;
  public var rutaActual:String;
  var estado:String;
  public var salir:Bool;

  var previewX:Int;
  var panelY:Int;
  var topY:Int;

  // Ids de comando del menu (mismos que filesapp.c).
  public static inline var CMD_REFRESCAR:Int = 1;
  public static inline var CMD_SUBIR:Int = 2;
  public static inline var CMD_ACERCA:Int = 8;
  public static inline var CMD_SALIR:Int = 9;

  public function new(p:Painter) {
    this.p = p;

    this.menubar = new Menubar();
    var archivo = new Menu("Archivo");
    archivo.agregar("Refrescar", CMD_REFRESCAR);
    archivo.agregar("Subir", CMD_SUBIR);
    archivo.agregarSeparador();
    archivo.agregar("Salir", CMD_SALIR);
    var ayuda = new Menu("Ayuda");
    ayuda.agregar("Acerca de Files", CMD_ACERCA);
    menubar.agregar(archivo);
    menubar.agregar(ayuda);

    // Layout como filesapp_layout de filesapp.c: la barra arriba y los paneles
    // debajo.
    this.topY = menubar.altura(p) + 6;
    this.panelY = topY + 42;
    var panelH = p.alto - panelY - 40;
    var listW = Std.int(p.ancho / 2) - 18;
    this.previewX = 12 + listW + 12;
    var previewW = p.ancho - previewX - 12;

    this.lista = new Listbox(12, panelY, listW, panelH);
    this.preview = new Textview(previewX, panelY, previewW, panelH);

    // Dialog "Acerca de" (mismas medidas que el de filesapp.c: cliente 280x96,
    // boton OK en (90,56)).
    this.dialog = new Dialog("Acerca de", 280, 96, 90, 56);
    dialog.lineas.push("SavanXP Files (Haxe)");
    dialog.lineas.push("Navega directorios y previsualiza archivos");

    this.rutaActual = "/";
    this.estado = "";
    this.salir = false;
    cargar("/");
  }

  // Despacho de comandos del menu por id.
  function comando(id:Int) {
    if (id == CMD_REFRESCAR) {
      cargar(rutaActual);
    } else if (id == CMD_SUBIR) {
      subir();
    } else if (id == CMD_ACERCA) {
      dialog.abrir(p);
    } else if (id == CMD_SALIR) {
      salir = true;
    }
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

    var full:String = untyped __cpp__(
      "std::string(sxn_fs_join({0}.c_str(), {1}.c_str()))", rutaActual, nombre);

    if (esdir) {
      cargar(full);
      return;
    }

    // Archivo: si vive en /bin o /disk/bin, se le pide al escritorio que lo
    // lance en otra ventana (canal fd 9); si no, solo refresca el preview.
    if (untyped __cpp__("sxn_fs_is_launchable({0}.c_str()) != 0", full)) {
      var r:Int = untyped __cpp__("(int)sxn_gui_launch({0}.c_str())", full);
      if (r != 0) {
        estado = "Fallo el lanzamiento";
        untyped __cpp__('sxn_log_num("files: launch fallo", {0})', r);
      } else {
        estado = "Lanzamiento pedido: " + nombre;
        var traza = "files: launch " + full;
        untyped __cpp__('sxn_log({0}.c_str())', traza);
      }
      return;
    }
    actualizarPreview();
    estado = "Preview actualizado";
  }

  function seleccionar(idx:Int) {
    lista.seleccion = idx;
    lista.asegurarVisible(p);
    actualizarPreview();
  }

  public function repintar() {
    p.rect(0, 0, p.ancho, p.alto, Painter.FACE);
    p.label(12, topY, "Ruta: " + rutaActual);
    p.label(12, panelY - 20, "Directorio");
    p.label(previewX, panelY - 20, "Preview");

    lista.dibujar(p);
    preview.dibujar(p);

    var sy = p.alto - 32;
    p.rect(12, sy, p.ancho - 24, 22, Painter.FACE);
    p.sunken(12, sy, p.ancho - 24, 22);
    p.label(16, sy + 2, estado);

    // La barra va al final: su popup tiene que quedar por encima de todo.
    menubar.dibujar(p);
    // ...y el dialog modal por encima incluso de la barra.
    dialog.dibujar(p);
  }

  public function puntero(mx:Int, my:Int, izq:Bool, izqPrev:Bool):Bool {
    // Modal: con el dialog abierto, solo el dialog recibe input.
    if (dialog.activo) {
      dialog.sincronizar(p);
      if (izq && !izqPrev) {
        if (dialog.ok.contiene(mx, my)) {
          dialog.ok.pressed = true;
          return true;
        }
      } else if (!izq && izqPrev) {
        if (dialog.ok.pressed) {
          dialog.ok.pressed = false;
          if (dialog.ok.contiene(mx, my)) {
            dialog.cerrar();
          }
          return true;
        }
      }
      return false;
    }

    var cambio = false;

    // Resaltado del item bajo el cursor mientras hay un menu desplegado.
    if (menubar.abierto >= 0) {
      var sobre = menubar.itemEn(p, mx, my);
      if (sobre != menubar.resaltado) {
        menubar.resaltado = sobre;
        cambio = true;
      }
    }

    if (izq && !izqPrev) {
      // Press: sobre un titulo abre/cierra; fuera del popup cierra.
      var titulo = menubar.tituloEn(p, mx, my);
      if (titulo >= 0) {
        if (menubar.abierto == titulo) {
          menubar.cerrar();
        } else {
          menubar.abierto = titulo;
          menubar.resaltado = -1;
        }
        return true;
      }
      if (menubar.abierto >= 0 && !menubar.enPopup(p, mx, my)) {
        menubar.cerrar();
        return true;
      }
      return cambio;
    }

    if (!izq && izqPrev) {
      // Release con un menu desplegado: sobre un item dispara el comando; sobre
      // el titulo no hace nada (es el release del click que lo abrio).
      if (menubar.abierto >= 0) {
        if (menubar.tituloEn(p, mx, my) >= 0) {
          return cambio;
        }
        var id = menubar.idEn(p, mx, my);
        menubar.cerrar();
        if (id >= 0) {
          comando(id);
        }
        return true;
      }
      // Click en la lista: selecciona; si ya estaba seleccionado, activa.
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
    return cambio;
  }

  public function tecla(key:Int):Bool {
    // Modal: con el dialog abierto, ESC/Enter lo cierran y nada mas pasa.
    if (dialog.activo) {
      if (key == 27 || key == 13) {
        dialog.cerrar();
        return true;
      }
      return false;
    }
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

  // Mismo tamano que la version en C: dos paneles (lista + preview) sin
  // comerse la pantalla. El layout se arma abajo con lo que conceda el WM.
  if (untyped __cpp__("(int)sxn_gui_request_content_size(640u, 420u)") == 0) {
    untyped __cpp__("sxn_gui_wait_content_size(250)");
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
