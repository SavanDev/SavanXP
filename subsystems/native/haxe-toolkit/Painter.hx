// SavanXP - toolkit sxgui nativo (Haxe): Painter.
//
// Superficie de dibujo sobre un Array<Int> XRGB contiguo (stride = pitch de la
// superficie del compositor). Paleta y biseles 3D estilo sxgui (Win9x), texto
// por la fuente Noto horneada (sxn_text_* del runtime nativo). Compartido por
// las apps GUI nativas (sxguiapp, aboutapp, ...) via -cp haxe-toolkit.

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

  // Rectangulo de 1px de contorno.
  public function marco(x:Int, y:Int, w:Int, h:Int, color:Int) {
    hline(x, y, w, color);
    hline(x, y + h - 1, w, color);
    vline(x, y, h, color);
    vline(x + w - 1, y, h, color);
  }

  // Borde 3D levantado (botones, cara de ventana).
  public function raised(x:Int, y:Int, w:Int, h:Int) {
    hline(x, y, w, LIGHT);
    vline(x, y, h, LIGHT);
    hline(x, y + h - 1, w, DARK);
    vline(x + w - 1, y, h, DARK);
    hline(x + 1, y + h - 2, w - 2, SHADOW);
    vline(x + w - 2, y + 1, h - 2, SHADOW);
  }

  // Borde 3D hundido (campos, botones presionados).
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

  // Etiqueta de texto simple (color TEXT).
  public function label(x:Int, y:Int, s:String) {
    texto(x, y, s, TEXT);
  }

  // Group box sxgui: marco etched (LIGHT sobre SHADOW) con la linea superior a
  // media altura del texto y el titulo montado encima (mismo look que sxgui.c).
  public function groupbox(x:Int, y:Int, w:Int, h:Int, titulo:String) {
    var th = altoTexto();
    var fy = y + Std.int(th / 2);
    var fh = h - Std.int(th / 2);
    marco(x + 1, fy + 1, w - 1, fh - 1, LIGHT);
    marco(x, fy, w - 1, fh - 1, SHADOW);
    var tx = x + 8;
    rect(tx - 3, y, anchoTexto(titulo) + 6, th, FACE); // limpia la linea detras del titulo
    texto(tx, y, titulo, TEXT);
  }

  public function present():Int {
    return untyped __cpp__("(int)sxn_gui_present(&(*{0})[0])", pixeles);
  }
}
