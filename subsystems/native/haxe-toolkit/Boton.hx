// SavanXP - toolkit sxgui nativo (Haxe): Boton.
//
// Widget de boton con estado pressed + hit-test. Se dibuja levantado
// (raised) o hundido (sunken) segun el estado; el label se desplaza 1px cuando
// esta presionado (look Win9x). Compartido por las apps GUI nativas.

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
    var desplazamiento = pressed ? 1 : 0;
    var tw = p.anchoTexto(label);
    var th = p.altoTexto();
    var tx = x + Std.int((w - tw) / 2) + desplazamiento;
    var ty = y + Std.int((h - th) / 2) + desplazamiento;
    p.texto(tx, ty, label, Painter.TEXT);
  }
}
