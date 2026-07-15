// SavanXP - toolkit sxgui nativo (Haxe): Textview.
//
// Panel de texto de solo lectura (campo hundido + lineas). Sin seleccion ni
// scroll por ahora: muestra las primeras lineas que entran. Lo usa el preview
// de filesapp.

class Textview {
  public var x:Int;
  public var y:Int;
  public var w:Int;
  public var h:Int;
  public var lines:Array<String>;

  public function new(x:Int, y:Int, w:Int, h:Int) {
    this.x = x;
    this.y = y;
    this.w = w;
    this.h = h;
    this.lines = [];
  }

  public function filasVisibles(p:Painter):Int {
    var rh = p.altoTexto();
    return rh > 0 ? Std.int((h - 4) / rh) : 0;
  }

  public function dibujar(p:Painter) {
    p.rect(x, y, w, h, Painter.FIELD);
    p.sunken(x, y, w, h);
    var rh = p.altoTexto();
    var vis = filasVisibles(p);
    var i = 0;
    while (i < vis && i < lines.length) {
      p.texto(x + 4, y + 2 + i * rh, lines[i], Painter.TEXT);
      i += 1;
    }
  }
}
