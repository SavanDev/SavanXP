// SavanXP - sxguiapp: app sxgui-style INTERACTIVA del subsistema nativo (Haxe).
// Fase 3 del port del escritorio.
//
// Usa el toolkit compartido (haxe-toolkit: Painter, Boton) via -cp. Modelo de
// widgets con estado, hit-test del puntero (canal fd 5), estados pressed/click
// con feedback visual y un event loop que repinta on-change. Cliente del
// compositor igual que nativegui; el escritorio pone la barra de titulo.
//
// Sin closures anidados (el codegen de reflaxe.CPP los prefiere como metodos).

// Estado + logica de la ventana sxgui.
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
    p.label(12, 8, "Panel nativo (Haxe)");

    // Campo hundido con el mensaje.
    p.rect(12, 34, p.ancho - 24, 26, Painter.FIELD);
    p.sunken(12, 34, p.ancho - 24, 26);
    p.label(17, 38, mensaje);

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

  public function puntero(mx:Int, my:Int, izq:Bool, izqPrev:Bool):Bool {
    if (izq && !izqPrev) {
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
      if (esc.puntero(mx, my, izq, izqPrev)) {
        cambio = true;
      }
      prevBtn = btn;
    }

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
