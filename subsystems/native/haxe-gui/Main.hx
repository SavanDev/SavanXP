// SavanXP - nativegui: primera app VENTANEADA del subsistema nativo.
//
// Cliente del compositor escrito en Haxe sobre la capa sxn_gui_* del runtime
// (protocolo de superficie v3 por los fds 3..9 que instala el shell antes del
// exec). Dibuja un patron reconocible, presenta varios frames y procesa algo
// de input; disenada para correr igual de bien bajo el escritorio real que
// bajo el harness headless (subsystems/native/test/guihost.c), que ademas
// espera que salga sola tras unos frames.
//
// Como en Main.hx de validacion: sin Float (division entera via __cpp__) y
// con las validaciones dinamicas en Haxe (los if sobreviven gracias a que
// SxnCompilerInit excluye RemovePureExpressions).

class Ventana {
  public var ancho:Int;
  public var alto:Int;
  public var stride:Int;
  public var pixeles:Array<Int>;

  public function new(ancho:Int, alto:Int, stride:Int) {
    this.ancho = ancho;
    this.alto = alto;
    this.stride = stride;
    this.pixeles = [];
    untyped __cpp__("{0}->resize((std::size_t)({1}))", pixeles, stride * alto);
  }

  public function rectangulo(x0:Int, y0:Int, w:Int, h:Int, color:Int) {
    for (fila in y0...(y0 + h)) {
      for (columna in x0...(x0 + w)) {
        if (fila >= 0 && fila < alto && columna >= 0 && columna < ancho) {
          pixeles[fila * stride + columna] = color;
        }
      }
    }
  }

  public function fondo() {
    for (fila in 0...alto) {
      for (columna in 0...ancho) {
        var azul:Int = untyped __cpp__("(({0} * 160) / {1})", fila, alto);
        pixeles[fila * stride + columna] = 0x102030 + azul;
      }
    }
  }

  public function presentar():Int {
    return untyped __cpp__("(int)sxn_gui_present(&(*{0})[0])", pixeles);
  }

  public function presentarRegion(x:Int, y:Int, w:Int, h:Int):Int {
    return untyped __cpp__(
      "(int)sxn_gui_present_region(&(*{0})[0], (unsigned){1}, (unsigned){2}, (unsigned){3}, (unsigned){4})",
      pixeles, x, y, w, h);
  }
}

function main() {
  var abierto:Int = untyped __cpp__("(int)sxn_gui_open()");
  if (abierto != 0) {
    untyped __cpp__('sxn_log_num("gui: sin sesion de ventana (no lanzado por el escritorio)", {0})', abierto);
    untyped __cpp__("sxn_exit(2)");
  }

  var ancho:Int = untyped __cpp__("(int)sxn_gui_width()");
  var alto:Int = untyped __cpp__("(int)sxn_gui_height()");
  var stride:Int = untyped __cpp__("(int)sxn_gui_stride_pixels()");
  untyped __cpp__('sxn_log_num("gui: ventana ancho", {0})', ancho);
  untyped __cpp__('sxn_log_num("gui: ventana alto", {0})', alto);

  var ventana = new Ventana(ancho, alto, stride);
  ventana.fondo();
  ventana.rectangulo(20, 20, 80, 50, 0x35C28D);

  var presento = ventana.presentar();
  if (presento != 0) {
    untyped __cpp__('sxn_log_num("gui: fallo el primer present", {0})', presento);
    untyped __cpp__("sxn_exit(1)");
  }

  // Unos frames animados: la caja avanza y cada present es una region sucia.
  var frames = 0;
  var x = 20;
  while (frames < 3) {
    if (untyped __cpp__("sxn_gui_should_close()")) {
      untyped __cpp__('sxn_log("gui: shutdown pedido por el compositor")');
      break;
    }

    ventana.rectangulo(x, 20, 80, 50, 0x102030);
    x += 24;
    ventana.rectangulo(x, 20, 80, 50, 0x35C28D);
    var resultado = ventana.presentarRegion(x - 24, 20, 80 + 24, 50);
    if (resultado != 0) {
      untyped __cpp__('sxn_log_num("gui: fallo present_region", {0})', resultado);
      untyped __cpp__("sxn_exit(1)");
    }
    frames += 1;
  }

  // Drenar input: el harness manda una tecla; bajo el escritorio puede no
  // haber nada encolado y esta bien.
  untyped __cpp__("struct sxn_gui_input_event __ev; int __ev_result = sxn_gui_poll_event(&__ev)");
  if (untyped __cpp__("__ev_result == 1")) {
    var tipo:Int = untyped __cpp__("(int)__ev.type");
    var tecla:Int = untyped __cpp__("(int)__ev.key");
    untyped __cpp__('sxn_log_num("gui: evento tipo", {0})', tipo);
    untyped __cpp__('sxn_log_num("gui: evento tecla", {0})', tecla);
  }

  var compuestos:Int = untyped __cpp__("(int)sxn_gui_composed_sequence()");
  untyped __cpp__('sxn_log_num("gui: frames compuestos", {0})', compuestos);

  untyped __cpp__("sxn_gui_close()");
  untyped __cpp__('sxn_log("gui: cliente de compositor OK")');
}
