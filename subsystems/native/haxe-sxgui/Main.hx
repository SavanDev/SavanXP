// SavanXP - sxguiapp: app sxgui-style INTERACTIVA del subsistema nativo (Haxe).
// Fase 3 del port del escritorio.
//
// Evoluciona la primera version estatica a una GUI de verdad: modelo de
// widgets (Boton con estado), hit-test del puntero (canal fd 5), estados
// pressed/click con feedback visual (bisel levantado -> hundido) y un event
// loop que repinta solo cuando algo cambia. Cliente del compositor igual que
// nativegui; el escritorio pone la barra de titulo. Bajo el harness headless
// (test/sxguihost.c) se lo maneja con eventos de puntero y se cierra por el
// evento de shutdown; bajo el escritorio real corre como ventana normal.
//
// Sin closures anidados (el codegen de reflaxe.CPP prefiere metodos de clase).

class Painter {
  public var ancho:Int;
  public var alto:Int;
  public var stride:Int;
  public var pixeles:Array<Int>;

  // Paleta sxgui (Win9x), misma que SXGUI_COLOR_* del SDK posix.
  public static inline var FACE:Int = 0xC0C0C0;
  public static inline var SHADOW:Int = 0x808080;
  public static inline var DARK:Int = 0x000000;
  public static inline var LIGHT:Int = 0xFFFFFF;
  public static inline var TEXT:Int = 0x000000;
  public static inline var FIELD:Int = 0xFFFFFF;

  public function new(ancho:Int, alto:Int, stride:Int) {
    this.ancho = ancho;
    this.alto = alto;
    this.stride = stride;
    this.pixeles = [];
    untyped __cpp__("{0}->resize((std::size_t)({1}))", pixeles, stride * alto);
  }

  public function rect(x:Int, y:Int, w:Int, h:Int, color:Int) {
    for (fila in y...(y + h)) {
      for (columna in x...(x + w)) {
        if (fila >= 0 && fila < alto && columna >= 0 && columna < ancho) {
          pixeles[fila * stride + columna] = color;
        }
      }
    }
  }

  public function hline(x:Int, y:Int, w:Int, color:Int) { rect(x, y, w, 1, color); }
  public function vline(x:Int, y:Int, h:Int, color:Int) { rect(x, y, 1, h, color); }

  public function raised(x:Int, y:Int, w:Int, h:Int) {
    hline(x, y, w, LIGHT);
    vline(x, y, h, LIGHT);
    hline(x, y + h - 1, w, DARK);
    vline(x + w - 1, y, h, DARK);
    hline(x + 1, y + h - 2, w - 2, SHADOW);
    vline(x + w - 2, y + 1, h - 2, SHADOW);
  }

  public function sunken(x:Int, y:Int, w:Int, h:Int) {
    hline(x, y, w, SHADOW);
    vline(x, y, h, SHADOW);
    hline(x, y + h - 1, w, LIGHT);
    vline(x + w - 1, y, h, LIGHT);
  }

  public function texto(x:Int, y:Int, s:String, color:Int) {
    untyped __cpp__(
      "sxn_text_draw((unsigned int*)&(*{0})[0], {1}, {2}, {3}, {4}, {5}, {6}.c_str(), (unsigned int){7})",
      pixeles, stride, ancho, alto, x, y, s, color);
  }

  public function anchoTexto(s:String):Int {
    return untyped __cpp__("sxn_text_width({0}.c_str())", s);
  }

  public function altoTexto():Int {
    return untyped __cpp__("sxn_text_height()");
  }

  public function present():Int {
    return untyped __cpp__("(int)sxn_gui_present(&(*{0})[0])", pixeles);
  }
}

class Boton {
  public var x:Int;
  public var y:Int;
  public var w:Int;
  public var h:Int;
  public var label:String;
  public var pressed:Bool;

  public function new(x:Int, y:Int, w:Int, h:Int, label:String) {
    this.x = x;
    this.y = y;
    this.w = w;
    this.h = h;
    this.label = label;
    this.pressed = false;
  }

  public function contiene(px:Int, py:Int):Bool {
    return px >= x && px < x + w && py >= y && py < y + h;
  }

  public function dibujar(p:Painter) {
    p.rect(x, y, w, h, Painter.FACE);
    if (pressed) {
      p.sunken(x, y, w, h);
    } else {
      p.raised(x, y, w, h);
    }
    var desplazamiento = pressed ? 1 : 0; // el label baja/derecha cuando esta hundido
    var tw = p.anchoTexto(label);
    var th = p.altoTexto();
    var tx = x + Std.int((w - tw) / 2) + desplazamiento;
    var ty = y + Std.int((h - th) / 2) + desplazamiento;
    p.texto(tx, ty, label, Painter.TEXT);
  }
}

// Estado + logica de la ventana sxgui (sin closures: todo en metodos).
class Escritorio {
  var p:Painter;
  var aceptar:Boton;
  var cancelar:Boton;
  var mensaje:String;
  var lampColor:Int;
  var capturado:Boton;
  var clicks:Int;

  public static inline var LAMP_X:Int = 228;
  public static inline var LAMP_Y:Int = 36;
  public static inline var LAMP_W:Int = 20;
  public static inline var LAMP_H:Int = 20;
  public static inline var VERDE:Int = 0x008000;
  public static inline var ROJO:Int = 0x800000;

  public function new(p:Painter) {
    this.p = p;
    this.aceptar = new Boton(16, 96, 96, 26, "Aceptar");
    this.cancelar = new Boton(124, 96, 96, 26, "Cancelar");
    this.mensaje = "sxgui-style desde el subsistema nativo";
    this.lampColor = Painter.FACE; // apagada
    this.capturado = null;
    this.clicks = 0;
  }

  public function repintar() {
    p.rect(0, 0, p.ancho, p.alto, Painter.FACE);
    p.texto(12, 8, "Panel nativo (Haxe)", Painter.TEXT);

    // Campo hundido con el mensaje.
    p.rect(12, 34, p.ancho - 24, 26, Painter.FIELD);
    p.sunken(12, 34, p.ancho - 24, 26);
    p.texto(17, 38, mensaje, Painter.TEXT);

    // "Lampara" de estado (se enciende al hacer click).
    p.rect(LAMP_X, LAMP_Y, LAMP_W, LAMP_H, lampColor);
    p.sunken(LAMP_X, LAMP_Y, LAMP_W, LAMP_H);

    aceptar.dibujar(p);
    cancelar.dibujar(p);
  }

  function accionar(b:Boton) {
    if (b == aceptar) {
      clicks += 1;
      mensaje = "Aceptado";
      lampColor = VERDE;
    } else {
      mensaje = "Cancelado";
      lampColor = ROJO;
    }
  }

  // Procesa un evento de puntero (coordenadas locales + mascara de botones).
  // Devuelve true si cambio algo que amerite repintar.
  public function puntero(mx:Int, my:Int, izq:Bool, izqPrev:Bool):Bool {
    if (izq && !izqPrev) {
      // Press: capturar el boton bajo el cursor y hundirlo.
      if (aceptar.contiene(mx, my)) {
        aceptar.pressed = true;
        capturado = aceptar;
        return true;
      } else if (cancelar.contiene(mx, my)) {
        cancelar.pressed = true;
        capturado = cancelar;
        return true;
      }
    } else if (!izq && izqPrev) {
      // Release: si se suelta sobre el mismo boton capturado, es un click.
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
}

function main() {
  var abierto:Int = untyped __cpp__("(int)sxn_gui_open()");
  if (abierto != 0) {
    untyped __cpp__('sxn_log_num("sxgui: sin sesion de ventana", {0})', abierto);
    untyped __cpp__("sxn_exit(2)");
  }

  var ancho:Int = untyped __cpp__("(int)sxn_gui_width()");
  var alto:Int = untyped __cpp__("(int)sxn_gui_height()");
  var stride:Int = untyped __cpp__("(int)sxn_gui_stride_pixels()");
  untyped __cpp__('sxn_log_num("sxgui: ventana ancho", {0})', ancho);
  untyped __cpp__('sxn_log_num("sxgui: ventana alto", {0})', alto);

  var p = new Painter(ancho, alto, stride);
  var esc = new Escritorio(p);
  esc.repintar();
  if (p.present() != 0) {
    untyped __cpp__('sxn_log("sxgui: fallo el primer present")');
    untyped __cpp__("sxn_exit(1)");
  }

  // Event loop: procesa puntero/teclado y repinta on-change hasta el shutdown.
  var prevBtn = 0;
  while (true) {
    if (untyped __cpp__("sxn_gui_should_close()")) {
      break;
    }

    var cambio = false;

    // Drenar todos los eventos de puntero encolados.
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
      if (esc.puntero(mx, my, izq, izqPrev)) {
        cambio = true;
      }
      prevBtn = btn;
    }

    // Teclado: ESC cierra.
    untyped __cpp__("struct sxn_gui_input_event __ev; int __er = sxn_gui_poll_event(&__ev)");
    if (untyped __cpp__("__er == 1 && __ev.type == SXN_GUI_EVENT_KEY_DOWN && (int)__ev.key == 27")) {
      break;
    }

    if (cambio) {
      esc.repintar();
      if (p.present() != 0) {
        untyped __cpp__('sxn_log("sxgui: fallo present en el loop")');
        untyped __cpp__("sxn_exit(1)");
      }
    }

    untyped __cpp__("sxn_sleep_ms(8)");
  }

  untyped __cpp__('sxn_log("sxgui: app interactiva OK")');
  untyped __cpp__("sxn_gui_close()");
}
