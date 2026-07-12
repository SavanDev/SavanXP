// SavanXP - sxguiapp: primera app estilo sxgui (Win9x) del subsistema nativo,
// escrita en Haxe. Arranque de la Fase 3 (port del escritorio a Haxe).
//
// Cliente del compositor (protocolo de superficie v3 por los fds 3..9, misma
// base que nativegui). Dibuja el AREA CLIENTE de una ventana: el escritorio
// pone el marco/barra de titulo alrededor. Usa un mini-toolkit Painter con la
// paleta y los biseles 3D de sxgui (raised/sunken) y texto real por la fuente
// Noto horneada (sxn_text_* del runtime nativo). Bajo el harness headless
// (test/sxguihost.c) presenta unos frames y sale; bajo el escritorio real corre
// como ventana normal.

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

  // Borde 3D levantado (botones, cara de ventana).
  public function raised(x:Int, y:Int, w:Int, h:Int) {
    hline(x, y, w, LIGHT);
    vline(x, y, h, LIGHT);
    hline(x, y + h - 1, w, DARK);
    vline(x + w - 1, y, h, DARK);
    hline(x + 1, y + h - 2, w - 2, SHADOW);
    vline(x + w - 2, y + 1, h - 2, SHADOW);
  }

  // Borde 3D hundido (campos, areas de texto).
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

  // Boton sxgui: cara FACE + bisel levantado + label centrado.
  public function boton(x:Int, y:Int, w:Int, h:Int, label:String) {
    rect(x, y, w, h, FACE);
    raised(x, y, w, h);
    var tw = anchoTexto(label);
    var th = altoTexto();
    var tx = x + Std.int((w - tw) / 2);   // centrado (Float real de Haxe)
    var ty = y + Std.int((h - th) / 2);
    texto(tx, ty, label, TEXT);
  }

  public function present():Int {
    return untyped __cpp__("(int)sxn_gui_present(&(*{0})[0])", pixeles);
  }
}

// Dibuja el area cliente de la ventana sxgui.
function dibujar(p:Painter) {
  p.rect(0, 0, p.ancho, p.alto, Painter.FACE);

  // Encabezado.
  p.texto(12, 8, "Panel nativo (Haxe)", Painter.TEXT);

  // Campo hundido con texto.
  p.rect(12, 34, p.ancho - 24, 26, Painter.FIELD);
  p.sunken(12, 34, p.ancho - 24, 26);
  p.texto(17, 38, "sxgui-style desde el subsistema nativo", Painter.TEXT);

  // Dos botones con bisel levantado.
  p.boton(16, 96, 96, 26, "Aceptar");
  p.boton(124, 96, 96, 26, "Cancelar");
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
  dibujar(p);

  // Prueba en vivo del render de texto: log del ancho de un string.
  untyped __cpp__('sxn_log_num("sxgui: ancho de Aceptar (px)", {0})', p.anchoTexto("Aceptar"));

  // Presentar unos frames (el harness espera 3); cortar si el compositor pide
  // cerrar. La ventana es estatica, cada present repite el mismo contenido.
  var frames = 0;
  while (frames < 3) {
    if (untyped __cpp__("sxn_gui_should_close()")) {
      untyped __cpp__('sxn_log("sxgui: shutdown pedido por el compositor")');
      break;
    }
    var r = p.present();
    if (r != 0) {
      untyped __cpp__('sxn_log_num("sxgui: fallo present", {0})', r);
      untyped __cpp__("sxn_exit(1)");
    }
    frames += 1;
  }

  // Drenar teclado (el harness manda una tecla; bajo el escritorio puede no
  // haber nada).
  untyped __cpp__("struct sxn_gui_input_event __ev; int __ev_result = sxn_gui_poll_event(&__ev)");
  if (untyped __cpp__("__ev_result == 1")) {
    untyped __cpp__('sxn_log_num("sxgui: tecla", (int)__ev.key)');
  }

  untyped __cpp__('sxn_log("sxgui: app sxgui-style OK")');
  untyped __cpp__("sxn_gui_close()");
}
